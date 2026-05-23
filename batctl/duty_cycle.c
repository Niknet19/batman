// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) B.A.T.M.A.N. contributors */

#include "main.h"

#include <errno.h>
#include <linux/genetlink.h>
#include <netlink/genl/genl.h>

#include "batman_adv.h"
#include "netlink.h"
#include "sys.h"

static struct simple_boolean_data duty_cycle;

static int print_duty_cycle(struct nl_msg *msg, void *arg) {
  struct nlattr *attrs[BATADV_ATTR_MAX + 1];
  struct nlmsghdr *nlh;
  struct genlmsghdr *ghdr;

  nlh = nlmsg_hdr(msg);
  ghdr = nlmsg_data(nlh);

  if (ghdr->cmd != BATADV_CMD_GET_MESH)
    return NL_OK;

  if (nla_parse(attrs, BATADV_ATTR_MAX, genlmsg_attrdata(ghdr, 0),
                genlmsg_len(ghdr), batadv_netlink_policy))
    return NL_OK;

  if (!attrs[BATADV_ATTR_DUTY_CYCLE])
    return NL_OK;

  printf("%u\n", nla_get_u8(attrs[BATADV_ATTR_DUTY_CYCLE]));

  return NL_STOP;
}

static int get_duty_cycle(struct state *state) {
  return sys_simple_nlquery(state, BATADV_CMD_GET_MESH, NULL, print_duty_cycle);
}

static int set_attrs_duty_cycle(struct nl_msg *msg, void *arg) {
  struct state *state = arg;
  struct settings_data *settings = state->cmd->arg;
  struct simple_boolean_data *data = settings->data;

  nla_put_u8(msg, BATADV_ATTR_DUTY_CYCLE, data->val);

  return 0;
}

static int set_duty_cycle(struct state *state) {
  return sys_simple_nlquery(state, BATADV_CMD_SET_MESH, set_attrs_duty_cycle,
                            NULL);
}

static struct settings_data batctl_settings_duty_cycle = {
    .data = &duty_cycle,
    .parse = parse_simple_boolean,
    .netlink_get = get_duty_cycle,
    .netlink_set = set_duty_cycle,
};

COMMAND_NAMED(SUBCOMMAND_MIF, duty_cycle, "dc", handle_sys_setting,
              COMMAND_FLAG_MESH_IFACE | COMMAND_FLAG_NETLINK,
              &batctl_settings_duty_cycle,
              "[value]           \tdisplay or modify duty cycle (0-100, "
              "100=always active)");