// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) B.A.T.M.A.N. contributors:
 *
 * Marek Lindner <marek.lindner@mailbox.org>
 *
 * License-Filename: LICENSES/preferred/GPL-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "sys.h"

static struct node_priority_data {
  uint8_t node_priority;
} node_priority;

static int parse_node_priority(struct state *state, int argc, char *argv[]) {
  struct settings_data *settings = state->cmd->arg;
  struct node_priority_data *data = settings->data;
  char *endptr;
  unsigned long val;

  if (argc != 2) {
    fprintf(stderr, "Error - incorrect number of arguments (expected 1)\n");
    return -EINVAL;
  }

  val = strtoul(argv[1], &endptr, 0);
  if (!endptr || *endptr != '\0') {
    fprintf(stderr, "Error - the supplied argument is invalid: %s\n", argv[1]);
    return -EINVAL;
  }

  if (val > 255) {
    fprintf(stderr, "Error - value too large (max 255): %s\n", argv[1]);
    return -ERANGE;
  }

  data->node_priority = (uint8_t)val;
  return 0;
}

static int print_node_priority(struct nl_msg *msg, void *arg) {
  struct nlattr *attrs[BATADV_ATTR_MAX + 1];
  struct nlmsghdr *nlh = nlmsg_hdr(msg);
  struct genlmsghdr *ghdr;
  int *result = arg;

  if (!genlmsg_valid_hdr(nlh, 0))
    return NL_OK;

  ghdr = nlmsg_data(nlh);

  if (nla_parse(attrs, BATADV_ATTR_MAX, genlmsg_attrdata(ghdr, 0),
                genlmsg_len(ghdr), batadv_netlink_policy)) {
    return NL_OK;
  }

  if (!attrs[BATADV_ATTR_NODE_PRIORITY])
    return NL_OK;

  printf("%u\n", nla_get_u8(attrs[BATADV_ATTR_NODE_PRIORITY]));

  *result = 0;
  return NL_STOP;
}

static int get_node_priority(struct state *state) {
  return sys_simple_nlquery(state, BATADV_CMD_GET_MESH, NULL,
                            print_node_priority);
}

static int set_attrs_node_priority(struct nl_msg *msg, void *arg) {
  struct state *state = arg;
  struct settings_data *settings = state->cmd->arg;
  struct node_priority_data *data = settings->data;

  nla_put_u8(msg, BATADV_ATTR_NODE_PRIORITY, data->node_priority);

  return 0;
}

static int set_node_priority(struct state *state) {
  return sys_simple_nlquery(state, BATADV_CMD_SET_MESH, set_attrs_node_priority,
                            NULL);
}

static struct settings_data batctl_settings_node_priority = {
    .data = &node_priority,
    .parse = parse_node_priority,
    .netlink_get = get_node_priority,
    .netlink_set = set_node_priority,
};

COMMAND_NAMED(SUBCOMMAND_MIF, node_priority, "np", handle_sys_setting,
              COMMAND_FLAG_MESH_IFACE | COMMAND_FLAG_NETLINK,
              &batctl_settings_node_priority,
              "[priority]        \tdisplay or modify node_priority setting");