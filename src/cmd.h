/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2024 Andrew Jeffery */

#ifndef CULVERT_CMD_H
#define CULVERT_CMD_H

#include "connection.h"

#include <argp.h>
#include <string.h>

#include "ccan/autodata/autodata.h"

struct cmd {
    const char *name;
    const char *parent; // NULL if top-level command
    const char *short_doc;
    int has_children; // 1 if it has subcommands, 0 otherwise
    int (*fn)(struct argp_state *);
};

AUTODATA_TYPE(cmds, struct cmd);
#define REGISTER_CMD(cmd) AUTODATA_SYM(cmds, cmd);

/**
 * Compare function for sorting commands.
 * The root commands will be sorted first, followed by the subcommands.
 */
static inline int cmd_cmp(const void *a, const void *b)
{
    const struct cmd * const *acmd = a;
    const struct cmd * const *bcmd = b;

    /* If parent is NULL, it is a top-level command */
    if ((*acmd)->parent == NULL && (*bcmd)->parent != NULL)
        return -1;
    if ((*acmd)->parent != NULL && (*bcmd)->parent == NULL)
        return 1;

    /**
     * If both are top-level or both are from the same subcommand,
     * sort alphabetically by name
     */
    if ((*acmd)->parent == (*bcmd)->parent ||
        ((*acmd)->parent != NULL && (*bcmd)->parent != NULL &&
         strcmp((*acmd)->parent, (*bcmd)->parent) == 0))
        return strcmp((*acmd)->name, (*bcmd)->name);

    /* Otherwise, sort by parent name */
    return strcmp((*acmd)->parent, (*bcmd)->parent);
}

/**
 * Parse everything after the `via` word.
 * The `via` word itself is not included in the arguments.
 * The arguments are expected to be in the following order:
 * 1. interface
 * 2. ip
 * 3. port
 * 4. username
 * 5. password
 *
 * @param argi The index of the `via` word in the argv array.
 * @param state The argp state.
 * @param args The connection arguments struct that should be modified.
 */
static int
cmd_parse_via(int argi, struct argp_state *state, struct connection_args *args)
{
    if (argi >= state->argc)
        return -EINVAL;

    /* global argc - already processed arguments - 1 (for the `via` word) */
    int argc = state->argc - argi - 1;

    /* Preflight validation if argc is either 1 or 5 */
    if (argc != 1 && argc != 5)
        return -EINVAL;

    args->interface = state->argv[argi + 1];

    if (argc == 1)
        return 0;

    args->ip = state->argv[argi + 2];
    args->port = atoi(state->argv[argi + 3]);
    args->username = state->argv[argi + 4];
    args->password = state->argv[argi + 5];

    return 0;
}
#endif
