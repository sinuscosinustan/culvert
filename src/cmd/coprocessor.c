// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2024 Code Construct

#include "arg_helper.h"
#include "bits.h"
#include "connection.h"
#include "compiler.h"
#include "host.h"
#include "log.h"
#include "rev.h"
#include "soc.h"
#include "soc/scu.h"
#include "soc/sdmc.h"

#include <argp.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COPROC_CACHED_MEM_SIZE (16 * 1024 * 1024)
#define COPROC_TOTAL_MEM_SIZE (32 * 1024 * 1024)

#define SCU_COPROC_CTRL 0xa00
#define SCU_COPROC_CTRL_RESET_ASSERT BIT(1)
#define SCU_COPROC_CTRL_EN BIT(0)

#define SCU_COPROC_MEM_BASE 0xa04
#define SCU_COPROC_IMEM_LIMIT 0xa08
#define SCU_COPROC_DMEM_LIMIT 0xa0c
#define SCU_COPROC_CACHE_RANGE 0xa40
#define SCU_COPROC_CACHE_1ST_16MB_EN BIT(0)
#define SCU_COPROC_CACHE_FUNC 0xa48
#define SCU_COPROC_CACHE_EN BIT(0)

static char global_doc[] =
    "\n"
    "Coprocessor command"
    "\v"
    "Supported commands:\n"
    "  run         Run the coprocessor\n"
    "  stop        Stop the coprocessor\n";

struct cmd_coprocessor_args {
    char *args[2];
};

struct cmd_copressor_run_args {
    unsigned long int mem_base;
    unsigned long int mem_size;
    struct connection_args connection;
};

struct cmd_coprocessor_stop_args {
    char *args[2];
};

static struct argp_option options[] = {
    {0},
};

static error_t parse_opt_run(int key, char *arg, struct argp_state *state)
{
    struct cmd_copressor_run_args *arguments = state->input;
    char *endp;

    if (key == ARGP_KEY_ARG && !strcmp(arg, "via")) {
        if (parse_via(state->next - 1, state, &arguments->connection))
            argp_error(state, "Failed to parse connection arguments. Returned code %d\n", errno);
        return 0;
    }

    switch (key) {
        case ARGP_KEY_ARG:
            switch (state->arg_num) {
                case 0:
                    errno = 0;
                    arguments->mem_base = strtoul(arg, &endp, 0);
                    if (arguments->mem_base == ULONG_MAX && errno)
                        argp_error(state, "Failed to parse coprocessor RAM base '%ld': %s",
                                   arguments->mem_base, strerror(errno));
                    else if (arg == endp || *endp)
                        argp_error(state, "Failed to parse coprocessor RAM base '%ld'",
                                   arguments->mem_base);
                    break;
                case 1:
                    errno = 0;
                    arguments->mem_size = strtoul(arg, &endp, 0);
                    if (arguments->mem_size == ULONG_MAX && errno)
                        argp_error(state, "Failed to parse coprocessor RAM size '%ld': %s",
                                   arguments->mem_size, strerror(errno));
                    else if (arg == endp || *endp)
                        argp_error(state, "Failed to parse coprocessor RAM size '%ld'",
                                   arguments->mem_size);

                    if (arguments->mem_size != COPROC_TOTAL_MEM_SIZE)
                        argp_error(state, "We currently only support assigning 32M of memory to the coprocessor\n");
                    break;
            }
            break;
        case ARGP_KEY_END:
            if (state->arg_num < 2)
                argp_usage(state);
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp_run = {
    options,
    parse_opt_run,
    "<ADDRESS> <LENGTH> [via INTERFACE [IP PORT USERNAME PASSWORD]]",
    "Run the coprocessor",
    NULL,
    NULL,
    NULL
};

int cmd_coprocessor_run(struct argp_state* state)
{
    struct host _host, *host = &_host;
    struct soc _soc, *soc = &_soc;
    struct soc_region dram;
    struct sdmc *sdmc;
    struct ahb *ahb;
    struct scu *scu;
    ssize_t src;
    int rc;

    struct subcommand run_cmd;
    struct cmd_copressor_run_args arguments = {0};
    parse_subcommand(&argp_run, "run", &arguments, state, &run_cmd);

    errno = 0;
    if ((rc = host_init_new(host, &arguments.connection)) < 0) {
        loge("Failed to initialise host interface: %d\n", rc);
        return EXIT_FAILURE;
    }

    if (!(ahb = host_get_ahb(host))) {
        loge("Failed to acquire AHB interface\n");
        rc = EXIT_FAILURE;
        goto cleanup_host;
    }

    if ((rc = soc_probe(soc, ahb)) < 0) {
        loge("Failed to probe SoC: %d\n", rc);
        rc = EXIT_FAILURE;
        goto cleanup_host;
    }

    if (soc_generation(soc) != ast_g6) {
        loge("We currently only support the AST2600-series coprocessor\n");
        rc = EXIT_FAILURE;
        goto cleanup_soc;
    }

    if (!(sdmc = sdmc_get(soc))) {
        loge("Failed to acquire SDRAM memory controller\n");
        rc = EXIT_FAILURE;
        goto cleanup_soc;
    }

    if ((rc = sdmc_get_dram(sdmc, &dram))) {
        loge("Failed to locate DRAM: %d\n", rc);
        rc = EXIT_FAILURE;
        goto cleanup_soc;
    }

#if ULONG_MAX > UINT32_MAX
    if (arguments.mem_base > UINT32_MAX) {
        loge("Provided RAM base 0x%ux exceeds SoC physical address space\n", arguments.mem_base);
        rc = EXIT_FAILURE;
        goto cleanup_soc;
    }
#endif

    if (((arguments.mem_base + arguments.mem_size) & UINT32_MAX) < arguments.mem_base) {
        loge("Invalid RAM region provided for coprocessor\n");
        rc = EXIT_FAILURE;
        goto cleanup_soc;
    }

    if (arguments.mem_base < dram.start ||
        (arguments.mem_base + arguments.mem_size) > (dram.start + dram.length)) {
        loge("Ill-formed RAM region provided for coprocessor\n");
        rc = EXIT_FAILURE;
        goto cleanup_soc;
    }

    if (!(scu = scu_get(soc))) {
        loge("Failed to acquire SCU driver\n");
        rc = EXIT_FAILURE;
        goto cleanup_soc;
    }

    /* 4.1.2 SSP Cache Programming Procedure
     *
     * 'AST2600 SECONDARY SERVICE PROCESSOR v0.1f.pdf'
     */
    /* 1. */
    if ((rc = scu_writel(scu, SCU_COPROC_CTRL, 0)) < 0) {
        loge("Failed to disable coprocoessor: %d\n", rc);
        rc = EXIT_FAILURE;
        goto cleanup_scu;
    }

    /* 2. */
    if ((rc = scu_writel(scu, SCU_COPROC_CTRL, SCU_COPROC_CTRL_RESET_ASSERT)) < 0) {
        loge("Failed to assert the coprocessor reset: %d", rc);
        rc = EXIT_FAILURE;
        goto cleanup_scu;
    }

    /* 3. */
    if ((src = soc_siphon_in(soc, arguments.mem_base, arguments.mem_size, STDIN_FILENO)) < 0) {
        loge("Failed to load coprocessor firmware to provided region: %d\n", src);
        rc = EXIT_FAILURE;
        goto cleanup_scu;
    }

        /* 4. */
    if (scu_writel(scu, SCU_COPROC_MEM_BASE, arguments.mem_base) ||
        /* 5. */
        scu_writel(scu, SCU_COPROC_IMEM_LIMIT,
                   arguments.mem_base + COPROC_CACHED_MEM_SIZE) ||
        /* 6. */
        scu_writel(scu, SCU_COPROC_DMEM_LIMIT,
                   arguments.mem_base + arguments.mem_size) ||
        /* 7. */
        scu_writel(scu, SCU_COPROC_CACHE_RANGE, SCU_COPROC_CACHE_1ST_16MB_EN) ||
        /* 8. */
        scu_writel(scu, SCU_COPROC_CACHE_FUNC, SCU_COPROC_CACHE_EN)) {
        loge("Failed to configure coprocessor control registers\n");
        rc = EXIT_FAILURE;
        goto cleanup_scu;
    }

    if (usleep(1000) == -1) {
        loge("Coprocessor reset pre-delay failed: %s", strerror(errno));
        rc = EXIT_FAILURE;
        goto cleanup_scu;
    }

    /* 9. */
    if ((rc = scu_writel(scu, SCU_COPROC_CTRL, 0)) < 0) {
        loge("Failed to disable coprocessor: %d\n", rc);
        rc = EXIT_FAILURE;
        goto cleanup_scu;
    }

    if (usleep(1000) == -1) {
        loge("Coprocessor reset post-delay failed: %s", strerror(errno));
        rc = EXIT_FAILURE;
        goto cleanup_scu;
    }

    /* 10. */
    if ((rc = scu_writel(scu, SCU_COPROC_CTRL, SCU_COPROC_CTRL_EN)) < 0) {
        loge("Failed to start coprocessor: %d\n", rc);
        rc = EXIT_FAILURE;
        goto cleanup_scu;
    }

    rc = EXIT_SUCCESS;

cleanup_scu:
    scu_put(scu);

cleanup_soc:
    soc_destroy(soc);

cleanup_host:
    host_destroy(host);

    return rc;
}

static struct argp argp_stop = {
    options,
    NULL,
    "[INTERFACE [IP PORT USERNAME PASSWORD]]",
    "Stop the coprocessor",
    NULL,
    NULL,
    NULL
};

int cmd_coprocessor_stop(struct argp_state* state)
{
    struct host _host, *host = &_host;
    struct soc _soc, *soc = &_soc;
    struct ahb *ahb;
    struct scu *scu;
    int rc;

    struct subcommand stop_cmd;
    struct cmd_coprocessor_stop_args arguments = {0};
    parse_subcommand(&argp_stop, "stop", &arguments, state, &stop_cmd);

    if ((rc = host_init(host, stop_cmd.argc, stop_cmd.argv)) < 0) {
        loge("Failed to initialise host interface: %d\n", rc);
        return EXIT_FAILURE;
    }

    if (!(ahb = host_get_ahb(host))) {
        loge("Failed to acquire AHB interface\n");
        rc = EXIT_FAILURE;
        goto cleanup_host;
    }

    if ((rc = soc_probe(soc, ahb)) < 0) {
        loge("Failed to probe SoC: %d\n", rc);
        rc = EXIT_FAILURE;
        goto cleanup_host;
    }

    if (soc_generation(soc) != ast_g6) {
        loge("We currently only support the AST2600-series coprocessor\n");
        rc = EXIT_FAILURE;
        goto cleanup_soc;
    }

    if (!(scu = scu_get(soc))) {
        loge("Failed to acquire SCU driver\n");
        rc = EXIT_FAILURE;
        goto cleanup_soc;
    }

    if ((rc = scu_writel(scu, SCU_COPROC_CTRL, 0)) < 0) {
        loge("Failed to disable coprocoessor: %d\n", rc);
        rc = EXIT_FAILURE;
    }

    scu_put(scu);

cleanup_soc:
    soc_destroy(soc);

cleanup_host:
    host_destroy(host);

    return rc;
}

static error_t parse_opt_global(int key, char *arg, struct argp_state *state)
{
    switch (key) {
        case ARGP_KEY_ARG:
            if (state->arg_num == 0 && !strcmp(arg, "run")) {
                cmd_coprocessor_run(state);
            } else if (state->arg_num == 0 && !strcmp(arg, "stop")) {
                cmd_coprocessor_stop(state);
            }
            break;
        case ARGP_KEY_END:
            if (state->arg_num < 1) {
                argp_usage(state);
            }
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp_global = {
    options,
    parse_opt_global,
    "<cmd> [CMD_OPTIONS]...",
    global_doc,
    NULL,
    NULL,
    NULL
};

int cmd_coprocessor(struct argp_state *state)
{
    struct subcommand global_cmd;
    struct cmd_coprocessor_args arguments = {0};
    parse_subcommand(&argp_global, "coprocessor", &arguments, state, &global_cmd);

    return EXIT_FAILURE;
}
