// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) B.A.T.M.A.N. contributors:
 *
 * Linus Lüssing, Marek Lindner
 */

#include "bat_v_elp.h"
#include "main.h"

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/byteorder/generic.h>
#include <linux/container_of.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/gfp.h>
#include <linux/if_ether.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/netdevice.h>
#include <linux/nl80211.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <uapi/linux/batadv_packet.h>

#include "bat_v_ogm.h"
#include "hard-interface.h"
#include "log.h"
#include "originator.h"
#include "routing.h"
#include "send.h"

/**
 * struct batadv_v_metric_queue_entry - list of hardif neighbors which require
 *  and metric update
 */
struct batadv_v_metric_queue_entry {
  /** @hardif_neigh: hardif neighbor scheduled for metric update */
  struct batadv_hardif_neigh_node *hardif_neigh;

  /** @list: list node for metric_queue */
  struct list_head list;
};

/**
 * batadv_v_elp_start_timer() - restart timer for ELP periodic work
 * @hard_iface: the interface for which the timer has to be reset
 */
static void batadv_v_elp_start_timer(struct batadv_hard_iface *hard_iface) {
  unsigned int msecs;

  msecs = atomic_read(&hard_iface->bat_v.elp_interval) - BATADV_JITTER;
  msecs += get_random_u32_below(2 * BATADV_JITTER);

  pr_err("=== AIRTIME DEBUG: ELP timer armed for %s, %u ms ===\n",
         hard_iface->net_dev->name, msecs);

  queue_delayed_work(batadv_event_workqueue, &hard_iface->bat_v.elp_wq,
                     msecs_to_jiffies(msecs));
}
/**
 * batadv_v_elp_airtime_get_base() - получить базовую airtime-стоимость линка
 * без учёта потерь пакетов
 * @neigh: сосед
 * @base_airtime: результат в микросекундах
 *
 * Return: true если значение получено, false если нужно использовать default
 */
static bool
batadv_v_elp_airtime_get_base(struct batadv_hardif_neigh_node *neigh,
                              u32 *base_airtime) {
  struct batadv_hard_iface *hard_iface = neigh->if_incoming;
  struct net_device *mesh_iface = hard_iface->mesh_iface;
  struct ethtool_link_ksettings link_settings;
  struct net_device *real_netdev;
  struct station_info sinfo;
  u32 bitrate = 0; /* бит/с */
  u32 throughput_override;
  int ret;

  if (!mesh_iface)
    return false;

  /* если пользователь задал значение вручную — используем его
   * (конвертируем из 100 kbit/s в бит/с)
   */
  throughput_override = atomic_read(&hard_iface->bat_v.throughput_override);
  if (throughput_override != 0) {
    bitrate = throughput_override * 100000;
    goto calculate;
  }

  /* для WiFi — запрашиваем через cfg80211 */
  if (batadv_is_wifi_hardif(hard_iface)) {
    if (!batadv_is_cfg80211_hardif(hard_iface))
      goto default_airtime;

    if (!rtnl_trylock())
      return false;
    real_netdev = __batadv_get_real_netdev(hard_iface->net_dev);
    rtnl_unlock();
    if (!real_netdev)
      goto default_airtime;

    ret = cfg80211_get_station(real_netdev, neigh->addr, &sinfo);
    if (!ret)
      cfg80211_sinfo_release_content(&sinfo);
    dev_put(real_netdev);

    if (ret == -ENOENT) {
      *base_airtime = BATADV_AIRTIME_MAX_VALUE;
      return true;
    }
    if (ret)
      goto default_airtime;

    if (sinfo.filled & BIT(NL80211_STA_INFO_EXPECTED_THROUGHPUT)) {
      bitrate = sinfo.expected_throughput * 1000;
      goto calculate;
    }

    if (sinfo.filled & BIT(NL80211_STA_INFO_TX_BITRATE)) {
      bitrate = cfg80211_calculate_bitrate(&sinfo.txrate) * 100000;
      goto calculate;
    }

    goto default_airtime;
  }

  /* для Ethernet — через ethtool */
  if (!rtnl_trylock())
    return false;

  ret = __ethtool_get_link_ksettings(hard_iface->net_dev, &link_settings);
  rtnl_unlock();
  if (ret == 0) {
    if (link_settings.base.duplex == DUPLEX_FULL)
      hard_iface->bat_v.flags |= BATADV_FULL_DUPLEX;
    else
      hard_iface->bat_v.flags &= ~BATADV_FULL_DUPLEX;

    if (link_settings.base.speed && link_settings.base.speed != SPEED_UNKNOWN) {
      bitrate = (u32)link_settings.base.speed * 1000000;
      goto calculate;
    }
  }

default_airtime:
  if (!(hard_iface->bat_v.flags & BATADV_WARNING_DEFAULT)) {
    batadv_info(mesh_iface,
                "Could not determine bitrate on %s, defaulting to 1 Mbps\n",
                hard_iface->net_dev->name);
    hard_iface->bat_v.flags |= BATADV_WARNING_DEFAULT;
  }
  /* default: 1 Mbps */
  bitrate = 1000000;

calculate:
  /* Формула 802.11s: base_airtime = O + B_t / r */
  if (bitrate > 0)
    *base_airtime = BATADV_AIRTIME_OVERHEAD +
                    (BATADV_AIRTIME_TEST_FRAME_SIZE * 1000000ULL) / bitrate;
  else
    *base_airtime = BATADV_AIRTIME_MAX_VALUE;

  pr_err("=== AIRTIME BASE: bitrate=%u, overhead=%u, frame_part=%llu, "
         "base_airtime=%u ===\n",
         bitrate, BATADV_AIRTIME_OVERHEAD,
         (BATADV_AIRTIME_TEST_FRAME_SIZE * 1000000ULL) / bitrate,
         *base_airtime);

  return true;
}

/**
 * batadv_v_elp_airtime_metric_update() - обновить airtime-метрику соседа
 * @neigh: сосед для обновления
 */
static void
batadv_v_elp_airtime_metric_update(struct batadv_hardif_neigh_node *neigh) {
  u32 base_airtime;
  u32 e_pt; /* частота ошибок кадра, 0-1000 (0.0% - 100.0%) */
  bool valid;

  valid = batadv_v_elp_airtime_get_base(neigh, &base_airtime);
  if (!valid)
    return;

  neigh->bat_v.base_airtime = base_airtime;

  /* вычисляем e_pt из счётчиков */
  if (neigh->bat_v.elp_expected > 0) {
    e_pt =
        1000 - (neigh->bat_v.elp_received * 1000 / neigh->bat_v.elp_expected);
  } else {
    e_pt = 0;
  }

  /* EWMA-сглаживание e_pt */
  neigh->bat_v.e_pt_smoothed =
      (e_pt + (BATADV_AIRTIME_EWMA_WEIGHT - 1) * neigh->bat_v.e_pt_smoothed) /
      BATADV_AIRTIME_EWMA_WEIGHT;

  /* сброс счётчиков */
  neigh->bat_v.elp_received = 0;
  neigh->bat_v.elp_expected = 0;

  /* итоговая airtime-стоимость = base_airtime / (1 - e_pt) */
  if (neigh->bat_v.e_pt_smoothed < 1000) {
    neigh->bat_v.airtime_cost =
        (base_airtime * 1000) / (1000 - neigh->bat_v.e_pt_smoothed);
  } else {
    neigh->bat_v.airtime_cost = BATADV_AIRTIME_MAX_VALUE;
  }
  pr_err(
      "=== AIRTIME METRIC: neighbor=%pM, base=%u, e_pt_raw=%u, e_pt_smooth=%u, "
      "received=%u, expected=%u, final_airtime=%u ===\n",
      neigh->addr, base_airtime, e_pt, neigh->bat_v.e_pt_smoothed,
      neigh->bat_v.elp_received, neigh->bat_v.elp_expected,
      neigh->bat_v.airtime_cost);
}

/**
 * batadv_v_elp_wifi_neigh_probe() - send link probing packets to a neighbour
 * @neigh: the neighbour to probe
 *
 * Sends a predefined number of unicast wifi packets to a given neighbour in
 * order to trigger the throughput estimation on this link by the RC algorithm.
 * Packets are sent only if there is not enough payload unicast traffic towards
 * this neighbour..
 *
 * Return: True on success and false in case of error during skb preparation.
 */
static bool
batadv_v_elp_wifi_neigh_probe(struct batadv_hardif_neigh_node *neigh) {
  struct batadv_hard_iface *hard_iface = neigh->if_incoming;
  struct batadv_priv *bat_priv = netdev_priv(hard_iface->mesh_iface);
  unsigned long last_tx_diff;
  struct sk_buff *skb;
  int probe_len, i;
  int elp_skb_len;

  /* this probing routine is for Wifi neighbours only */
  if (!batadv_is_wifi_hardif(hard_iface))
    return true;

  /* probe the neighbor only if no unicast packets have been sent
   * to it in the last 100 milliseconds: this is the rate control
   * algorithm sampling interval (minstrel). In this way, if not
   * enough traffic has been sent to the neighbor, batman-adv can
   * generate 2 probe packets and push the RC algorithm to perform
   * the sampling
   */
  last_tx_diff = jiffies_to_msecs(jiffies - neigh->bat_v.last_unicast_tx);
  if (last_tx_diff <= BATADV_ELP_PROBE_MAX_TX_DIFF)
    return true;

  probe_len =
      max_t(int, sizeof(struct batadv_elp_packet), BATADV_ELP_MIN_PROBE_SIZE);

  for (i = 0; i < BATADV_ELP_PROBES_PER_NODE; i++) {
    elp_skb_len = hard_iface->bat_v.elp_skb->len;
    skb = skb_copy_expand(hard_iface->bat_v.elp_skb, 0, probe_len - elp_skb_len,
                          GFP_ATOMIC);
    if (!skb)
      return false;

    /* Tell the skb to get as big as the allocated space (we want
     * the packet to be exactly of that size to make the link
     * throughput estimation effective.
     */
    skb_put_zero(skb, probe_len - hard_iface->bat_v.elp_skb->len);

    batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
               "Sending unicast (probe) ELP packet on interface %s to %pM\n",
               hard_iface->net_dev->name, neigh->addr);

    batadv_send_skb_packet(skb, hard_iface, neigh->addr);
  }

  return true;
}

/**
 * batadv_v_elp_periodic_work() - ELP periodic task per interface
 * @work: work queue item
 *
 * Emits broadcast ELP messages in regular intervals.
 */
static void batadv_v_elp_periodic_work(struct work_struct *work) {
  struct batadv_v_metric_queue_entry *metric_entry;
  struct batadv_v_metric_queue_entry *metric_safe;
  struct batadv_hardif_neigh_node *hardif_neigh;
  struct batadv_hard_iface *hard_iface;
  struct batadv_hard_iface_bat_v *bat_v;
  struct batadv_elp_packet *elp_packet;
  struct list_head metric_queue;
  struct batadv_priv *bat_priv;
  struct sk_buff *skb;
  u32 elp_interval;

  bat_v = container_of(work, struct batadv_hard_iface_bat_v, elp_wq.work);
  hard_iface = container_of(bat_v, struct batadv_hard_iface, bat_v);
  bat_priv = netdev_priv(hard_iface->mesh_iface);

  pr_err("=== AIRTIME DEBUG: ELP periodic work START for %s ===\n",
         hard_iface->net_dev->name);

  if (atomic_read(&bat_priv->mesh_state) == BATADV_MESH_DEACTIVATING) {
    pr_err("=== AIRTIME DEBUG: mesh DEACTIVATING, skipping ===\n");
    goto out;
  }

  /* we are in the process of shutting this interface down */
  if (hard_iface->if_status == BATADV_IF_NOT_IN_USE ||
      hard_iface->if_status == BATADV_IF_TO_BE_REMOVED) {
    pr_err("=== AIRTIME DEBUG: iface not in use, status=%d, skipping ===\n",
           hard_iface->if_status);
    goto out;
  }

  /* the interface was enabled but may not be ready yet */
  if (hard_iface->if_status != BATADV_IF_ACTIVE) {
    pr_err("=== AIRTIME DEBUG: iface not ACTIVE yet, status=%d ===\n",
           hard_iface->if_status);
    goto restart_timer;
  }

  skb = skb_copy(hard_iface->bat_v.elp_skb, GFP_ATOMIC);
  if (!skb) {
    pr_err("=== AIRTIME DEBUG: skb_copy failed! ===\n");
    goto restart_timer;
  }
  elp_packet = (struct batadv_elp_packet *)skb->data;
  elp_packet->seqno = htonl(atomic_read(&hard_iface->bat_v.elp_seqno));
  elp_interval = atomic_read(&hard_iface->bat_v.elp_interval);
  elp_packet->elp_interval = htonl(elp_interval);

  batadv_dbg(BATADV_DBG_BATMAN, bat_priv,
             "Sending broadcast ELP packet on interface %s, seqno %u\n",
             hard_iface->net_dev->name,
             atomic_read(&hard_iface->bat_v.elp_seqno));

  pr_err("=== AIRTIME DEBUG: SENDING broadcast ELP on %s, seqno=%u ===\n",
         hard_iface->net_dev->name, atomic_read(&hard_iface->bat_v.elp_seqno));

  batadv_send_broadcast_skb(skb, hard_iface);

  atomic_inc(&hard_iface->bat_v.elp_seqno);

  INIT_LIST_HEAD(&metric_queue);

  /* The throughput metric is updated on each sent packet. This way, if a
   * node is dead and no longer sends packets, batman-adv is still able to
   * react timely to its death.
   *
   * The throughput metric is updated by following these steps:
   * 1) if the hard_iface is wifi => send a number of unicast ELPs for
   *    probing/sampling to each neighbor
   * 2) update the throughput metric value of each neighbor (note that the
   *    value retrieved in this step might be 100ms old because the
   *    probing packets at point 1) could still be in the HW queue)
   */
  rcu_read_lock();
  hlist_for_each_entry_rcu(hardif_neigh, &hard_iface->neigh_list, list) {
    if (!batadv_v_elp_wifi_neigh_probe(hardif_neigh))
      /* if something goes wrong while probing, better to stop
       * sending packets immediately and reschedule the task
       */
      break;

    if (!kref_get_unless_zero(&hardif_neigh->refcount))
      continue;

    /* Reading the estimated throughput from cfg80211 is a task that
     * may sleep and that is not allowed in an rcu protected
     * context. Therefore add it to metric_queue and process it
     * outside rcu protected context.
     */
    metric_entry = kzalloc_obj(*metric_entry, GFP_ATOMIC);
    if (!metric_entry) {
      batadv_hardif_neigh_put(hardif_neigh);
      continue;
    }

    metric_entry->hardif_neigh = hardif_neigh;
    list_add(&metric_entry->list, &metric_queue);
  }
  rcu_read_unlock();

  list_for_each_entry_safe(metric_entry, metric_safe, &metric_queue, list) {
    batadv_v_elp_airtime_metric_update(metric_entry->hardif_neigh);

    batadv_hardif_neigh_put(metric_entry->hardif_neigh);
    list_del(&metric_entry->list);
    kfree(metric_entry);
  }

restart_timer:
  batadv_v_elp_start_timer(hard_iface);
out:
  return;
}

/**
 * batadv_v_elp_iface_enable() - setup the ELP interface private resources
 * @hard_iface: interface for which the data has to be prepared
 *
 * Return: 0 on success or a -ENOMEM in case of failure.
 */
int batadv_v_elp_iface_enable(struct batadv_hard_iface *hard_iface) {
  static const size_t tvlv_padding = sizeof(__be32);
  struct batadv_elp_packet *elp_packet;
  unsigned char *elp_buff;
  u32 random_seqno;
  size_t size;
  int res = -ENOMEM;

  size = ETH_HLEN + NET_IP_ALIGN + BATADV_ELP_HLEN + tvlv_padding;
  hard_iface->bat_v.elp_skb = dev_alloc_skb(size);
  if (!hard_iface->bat_v.elp_skb)
    goto out;

  skb_reserve(hard_iface->bat_v.elp_skb, ETH_HLEN + NET_IP_ALIGN);
  elp_buff =
      skb_put_zero(hard_iface->bat_v.elp_skb, BATADV_ELP_HLEN + tvlv_padding);
  elp_packet = (struct batadv_elp_packet *)elp_buff;

  elp_packet->packet_type = BATADV_ELP;
  elp_packet->version = BATADV_COMPAT_VERSION;

  /* randomize initial seqno to avoid collision */
  get_random_bytes(&random_seqno, sizeof(random_seqno));
  atomic_set(&hard_iface->bat_v.elp_seqno, random_seqno);

  /* assume full-duplex by default */
  hard_iface->bat_v.flags |= BATADV_FULL_DUPLEX;

  /* warn the user (again) if there is nox throughput data is available */
  hard_iface->bat_v.flags &= ~BATADV_WARNING_DEFAULT;

  if (batadv_is_wifi_hardif(hard_iface))
    hard_iface->bat_v.flags &= ~BATADV_FULL_DUPLEX;

  pr_err("=== AIRTIME DEBUG: batadv_v_elp_iface_enable for %s, elp_interval=%d "
         "===\n",
         hard_iface->net_dev->name,
         atomic_read(&hard_iface->bat_v.elp_interval));

  INIT_DELAYED_WORK(&hard_iface->bat_v.elp_wq, batadv_v_elp_periodic_work);
  batadv_v_elp_start_timer(hard_iface);

  pr_err("=== AIRTIME DEBUG: ELP timer started for %s ===\n",
         hard_iface->net_dev->name);
  res = 0;

out:
  return res;
}

/**
 * batadv_v_elp_iface_disable() - release ELP interface private resources
 * @hard_iface: interface for which the resources have to be released
 */
void batadv_v_elp_iface_disable(struct batadv_hard_iface *hard_iface) {
  cancel_delayed_work_sync(&hard_iface->bat_v.elp_wq);

  dev_kfree_skb(hard_iface->bat_v.elp_skb);
  hard_iface->bat_v.elp_skb = NULL;
}

/**
 * batadv_v_elp_iface_activate() - update the ELP buffer belonging to the given
 *  hard-interface
 * @primary_iface: the new primary interface
 * @hard_iface: interface holding the to-be-updated buffer
 */
void batadv_v_elp_iface_activate(struct batadv_hard_iface *primary_iface,
                                 struct batadv_hard_iface *hard_iface) {
  struct batadv_elp_packet *elp_packet;
  struct sk_buff *skb;

  if (!hard_iface->bat_v.elp_skb)
    return;

  skb = hard_iface->bat_v.elp_skb;
  elp_packet = (struct batadv_elp_packet *)skb->data;
  ether_addr_copy(elp_packet->orig, primary_iface->net_dev->dev_addr);
}

/**
 * batadv_v_elp_primary_iface_set() - change internal data to reflect the new
 *  primary interface
 * @primary_iface: the new primary interface
 */
void batadv_v_elp_primary_iface_set(struct batadv_hard_iface *primary_iface) {
  struct batadv_hard_iface *hard_iface;
  struct list_head *iter;

  /* update orig field of every elp iface belonging to this mesh */
  rcu_read_lock();
  netdev_for_each_lower_private_rcu(primary_iface->mesh_iface, hard_iface, iter)
      batadv_v_elp_iface_activate(primary_iface, hard_iface);
  rcu_read_unlock();
}

/**
 * batadv_v_elp_neigh_update() - update an ELP neighbour node
 * @bat_priv: the bat priv with all the mesh interface information
 * @neigh_addr: the neighbour interface address
 * @if_incoming: the interface the packet was received through
 * @elp_packet: the received ELP packet
 *
 * Updates the ELP neighbour node state with the data received within the new
 * ELP packet.
 */
static void batadv_v_elp_neigh_update(struct batadv_priv *bat_priv,
                                      u8 *neigh_addr,
                                      struct batadv_hard_iface *if_incoming,
                                      struct batadv_elp_packet *elp_packet) {
  struct batadv_neigh_node *neigh;
  struct batadv_orig_node *orig_neigh;
  struct batadv_hardif_neigh_node *hardif_neigh;
  s32 seqno_diff;
  s32 elp_latest_seqno;

  pr_err("=== AIRTIME DEBUG: neigh_update for %pM via %s ===\n", neigh_addr,
         if_incoming->net_dev->name);

  orig_neigh = batadv_v_ogm_orig_get(bat_priv, elp_packet->orig);
  if (!orig_neigh) {
    pr_err("=== AIRTIME DEBUG: FAILED to get/create orig for %pM ===\n",
           elp_packet->orig);
    return;
  }
  pr_err("=== AIRTIME DEBUG: orig_node for %pM OK ===\n", elp_packet->orig);

  neigh = batadv_neigh_node_get_or_create(orig_neigh, if_incoming, neigh_addr);
  if (!neigh) {
    pr_err("=== AIRTIME DEBUG: FAILED to get/create neigh for %pM ===\n",
           neigh_addr);
    goto orig_free;
  }
  pr_err("=== AIRTIME DEBUG: neigh_node for %pM OK ===\n", neigh_addr);

  hardif_neigh = batadv_hardif_neigh_get(if_incoming, neigh_addr);
  if (!hardif_neigh) {
    pr_err("=== AIRTIME DEBUG: FAILED to get hardif_neigh for %pM! "
           "Neighbor not in hardif_neigh list yet? ===\n",
           neigh_addr);
    pr_err("=== AIRTIME DEBUG: This means ELP probe didn't create hardif_neigh "
           "===\n");
    goto neigh_free;
  }
  pr_err("=== AIRTIME DEBUG: hardif_neigh found, airtime_cost=%u ===\n",
         hardif_neigh->bat_v.airtime_cost);

  {
    u32 new_seqno = ntohl(elp_packet->seqno);
    s32 gap;

    pr_err("=== AIRTIME DEBUG: seqno: new=%u, last=%u ===\n", new_seqno,
           hardif_neigh->bat_v.elp_last_seqno);

    /* если это не первый пакет от соседа — считаем разрыв */
    if (hardif_neigh->bat_v.elp_last_seqno > 0) {
      gap = (s32)(new_seqno - hardif_neigh->bat_v.elp_last_seqno) - 1;
      if (gap < 0)
        gap = 0;
      if (gap > 100)
        gap = 0;

      pr_err("=== AIRTIME DEBUG: gap=%d, elp_expected was %u, elp_received was "
             "%u ===\n",
             gap, hardif_neigh->bat_v.elp_expected,
             hardif_neigh->bat_v.elp_received);

      hardif_neigh->bat_v.elp_expected += 1 + gap;
      hardif_neigh->bat_v.elp_received += 1;
    } else {
      pr_err("=== AIRTIME DEBUG: first ELP from this neighbor ===\n");
      hardif_neigh->bat_v.elp_expected = 1;
      hardif_neigh->bat_v.elp_received = 1;
    }
    hardif_neigh->bat_v.elp_last_seqno = new_seqno;

    pr_err("=== AIRTIME DEBUG: updated: expected=%u, received=%u ===\n",
           hardif_neigh->bat_v.elp_expected, hardif_neigh->bat_v.elp_received);
  }

  elp_latest_seqno = hardif_neigh->bat_v.elp_latest_seqno;
  seqno_diff = ntohl(elp_packet->seqno) - elp_latest_seqno;

  pr_err("=== AIRTIME DEBUG: seqno_diff=%d (new=%u, latest=%u) ===\n",
         seqno_diff, ntohl(elp_packet->seqno), elp_latest_seqno);

  /* known or older sequence numbers are ignored. However always adopt
   * if the router seems to have been restarted.
   */
  if (seqno_diff < 1 && seqno_diff > -BATADV_ELP_MAX_AGE) {
    pr_err("=== AIRTIME DEBUG: OLD seqno, ignoring (diff=%d, max_age=%d) ===\n",
           seqno_diff, BATADV_ELP_MAX_AGE);
    goto hardif_free;
  }

  pr_err("=== AIRTIME DEBUG: seqno is NEW, updating neighbor ===\n");

  neigh->last_seen = jiffies;
  hardif_neigh->last_seen = jiffies;
  hardif_neigh->bat_v.elp_latest_seqno = ntohl(elp_packet->seqno);
  hardif_neigh->bat_v.elp_interval = ntohl(elp_packet->elp_interval);

  pr_err("=== AIRTIME DEBUG: neighbor updated: last_seen=%lu, elp_interval=%u "
         "===\n",
         hardif_neigh->last_seen, hardif_neigh->bat_v.elp_interval);

hardif_free:
  batadv_hardif_neigh_put(hardif_neigh);
neigh_free:
  batadv_neigh_node_put(neigh);
orig_free:
  batadv_orig_node_put(orig_neigh);
  pr_err("=== AIRTIME DEBUG: neigh_update FINISHED ===\n");
}

int batadv_v_elp_packet_recv(struct sk_buff *skb,
                             struct batadv_hard_iface *if_incoming) {
  struct batadv_priv *bat_priv = netdev_priv(if_incoming->mesh_iface);
  struct batadv_elp_packet *elp_packet;
  struct batadv_hard_iface *primary_if;
  struct ethhdr *ethhdr;
  bool res;
  int ret = NET_RX_DROP;

  pr_err("=== AIRTIME DEBUG: ELP RECEIVED on %s ===\n",
         if_incoming->net_dev->name);

  res = batadv_check_management_packet(skb, if_incoming, BATADV_ELP_HLEN);
  if (!res) {
    pr_err("=== AIRTIME DEBUG: ELP check_management_packet FAILED ===\n");
    goto free_skb;
  }
  pr_err("=== AIRTIME DEBUG: ELP check_management_packet OK ===\n");

  ethhdr = eth_hdr(skb);

  pr_err("=== AIRTIME DEBUG: ELP from %pM to %pM ===\n", ethhdr->h_source,
         ethhdr->h_dest);

  if (batadv_is_my_mac(bat_priv, ethhdr->h_source)) {
    pr_err("=== AIRTIME DEBUG: ELP is my own MAC, dropping ===\n");
    goto free_skb;
  }

  /* did we receive a B.A.T.M.A.N. V ELP packet on an interface
   * that does not have B.A.T.M.A.N. V ELP enabled ?
   */
  if (strcmp(bat_priv->algo_ops->name, "BATMAN_V") != 0) {
    pr_err("=== AIRTIME DEBUG: algo is NOT BATMAN_V (got '%s'), dropping ===\n",
           bat_priv->algo_ops->name);
    goto free_skb;
  }
  pr_err("=== AIRTIME DEBUG: algo is BATMAN_V, OK ===\n");

  elp_packet = (struct batadv_elp_packet *)skb->data;

  pr_err("=== AIRTIME DEBUG: ELP packet: seqno=%u, elp_interval=%u, orig=%pM "
         "===\n",
         ntohl(elp_packet->seqno), ntohl(elp_packet->elp_interval),
         elp_packet->orig);

  primary_if = batadv_primary_if_get_selected(bat_priv);
  if (!primary_if) {
    pr_err("=== AIRTIME DEBUG: NO PRIMARY IF, dropping ELP! ===\n");
    goto free_skb;
  }
  pr_err("=== AIRTIME DEBUG: primary_if=%s ===\n", primary_if->net_dev->name);

  pr_err("=== AIRTIME DEBUG: calling batadv_v_elp_neigh_update ===\n");
  batadv_v_elp_neigh_update(bat_priv, ethhdr->h_source, if_incoming,
                            elp_packet);
  pr_err("=== AIRTIME DEBUG: neigh_update returned ===\n");

  ret = NET_RX_SUCCESS;
  batadv_hardif_put(primary_if);

free_skb:
  pr_err("=== AIRTIME DEBUG: ELP packet %s (ret=%d) ===\n",
         ret == NET_RX_SUCCESS ? "ACCEPTED" : "DROPPED", ret);

  if (ret == NET_RX_SUCCESS)
    consume_skb(skb);
  else
    kfree_skb(skb);

  return ret;
}
