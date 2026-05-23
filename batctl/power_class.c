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

static struct power_class_data {
  uint8_t power_class;
} power_class;

static int parse_power_class(struct state *state, int argc, char *argv[]) {
  struct settings_data *settings = state->cmd->arg;
  struct power_class_data *data = settings->data;
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

  if (val < 1 || val > 3) {
    fprintf(stderr, "Error - power_class must be between 1 and 3\n");
    return -ERANGE;
  }

  data->power_class = (uint8_t)val;
  return 0;
}

static int print_power_class(struct nl_msg *msg, void *arg) {
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

  if (!attrs[BATADV_ATTR_POWER_CLASS])
    return NL_OK;

  printf("%u\n", nla_get_u8(attrs[BATADV_ATTR_POWER_CLASS]));

  *result = 0;
  return NL_STOP;
}

static int get_power_class(struct state *state) {
  return sys_simple_nlquery(state, BATADV_CMD_GET_MESH, NULL,
                            print_power_class);
}

static int set_attrs_power_class(struct nl_msg *msg, void *arg) {
  struct state *state = arg;
  struct settings_data *settings = state->cmd->arg;
  struct power_class_data *data = settings->data;

  nla_put_u8(msg, BATADV_ATTR_POWER_CLASS, data->power_class);

  return 0;
}

static int set_power_class(struct state *state) {
  return sys_simple_nlquery(state, BATADV_CMD_SET_MESH, set_attrs_power_class,
                            NULL);
}

static struct settings_data batctl_settings_power_class = {
    .data = &power_class,
    .parse = parse_power_class,
    .netlink_get = get_power_class,
    .netlink_set = set_power_class,
};

COMMAND_NAMED(SUBCOMMAND_MIF, power_class, "pc", handle_sys_setting,
              COMMAND_FLAG_MESH_IFACE | COMMAND_FLAG_NETLINK,
              &batctl_settings_power_class,
              "[class]          \tdisplay or modify power_class setting (1-3)");