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

static struct duty_cycle_data {
  uint8_t duty_cycle;
} duty_cycle;

static int parse_duty_cycle(struct state *state, int argc, char *argv[]) {
  struct settings_data *settings = state->cmd->arg;
  struct duty_cycle_data *data = settings->data;
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

  if (val > 100) {
    fprintf(stderr, "Error - duty_cycle must be between 0 and 100\n");
    return -ERANGE;
  }

  data->duty_cycle = (uint8_t)val;
  return 0;
}

static int print_duty_cycle(struct nl_msg *msg, void *arg) {
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

  if (!attrs[BATADV_ATTR_DUTY_CYCLE])
    return NL_OK;

  printf("%u\n", nla_get_u8(attrs[BATADV_ATTR_DUTY_CYCLE]));

  *result = 0;
  return NL_STOP;
}

static int get_duty_cycle(struct state *state) {
  return sys_simple_nlquery(state, BATADV_CMD_GET_MESH, NULL, print_duty_cycle);
}

static int set_attrs_duty_cycle(struct nl_msg *msg, void *arg) {
  struct state *state = arg;
  struct settings_data *settings = state->cmd->arg;
  struct duty_cycle_data *data = settings->data;

  nla_put_u8(msg, BATADV_ATTR_DUTY_CYCLE, data->duty_cycle);

  return 0;
}

static int set_duty_cycle(struct state *state) {
  return sys_simple_nlquery(state, BATADV_CMD_SET_MESH, set_attrs_duty_cycle,
                            NULL);
}

static struct settings_data batctl_settings_duty_cycle = {
    .data = &duty_cycle,
    .parse = parse_duty_cycle,
    .netlink_get = get_duty_cycle,
    .netlink_set = set_duty_cycle,
};

COMMAND_NAMED(
    SUBCOMMAND_MIF, duty_cycle, "dc", handle_sys_setting,
    COMMAND_FLAG_MESH_IFACE | COMMAND_FLAG_NETLINK, &batctl_settings_duty_cycle,
    "[value]          \tdisplay or modify duty_cycle setting (0-100)");