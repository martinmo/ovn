/*
 * Copyright (c) 2020 eBay Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <ctype.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>

#include "command-line.h"
#include "compiler.h"
#include "db-ctl-base.h"
#include "ovn-dbctl.h"
#include "dirs.h"
#include "fatal-signal.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/json.h"
#include "openvswitch/shash.h"
#include "openvswitch/vconn.h"
#include "openvswitch/vlog.h"
#include "lib/ovn-ic-sb-idl.h"
#include "lib/ovn-util.h"
#include "openvswitch/poll-loop.h"
#include "process.h"
#include "sset.h"
#include "stream-ssl.h"
#include "stream.h"
#include "table.h"
#include "util.h"
#include "svec.h"

VLOG_DEFINE_THIS_MODULE(ic_sbctl);

struct ic_sbctl_context;

/* --db: The database server to contact. */
static const char *db;

/* --oneline: Write each command's output as a single line? */
static bool oneline;

/* --dry-run: Do not commit any changes. */
static bool dry_run;

/* --timeout: Time to wait for a connection to 'db'. */
static unsigned int timeout;

/* Format for table output. */
static struct table_style table_style = TABLE_STYLE_DEFAULT;

/* The IDL we're using and the current transaction, if any.
 * This is for use by ic_sbctl_exit() only, to allow it to clean up.
 * Other code should use its context arguments. */
static struct ovsdb_idl *the_idl;
static struct ovsdb_idl_txn *the_idl_txn;
OVS_NO_RETURN static void ic_sbctl_exit(int status);

/* --leader-only, --no-leader-only: Only accept the leader in a cluster. */
static int leader_only = true;

static void ic_sbctl_cmd_init(void);
OVS_NO_RETURN static void usage(void);
static void parse_options(int argc, char *argv[], struct shash *local_options);
static void run_prerequisites(struct ctl_command[], size_t n_commands,
                              struct ovsdb_idl *);
static bool do_ic_sbctl(const char *args, struct ctl_command *, size_t n,
                     struct ovsdb_idl *);

int
main(int argc, char *argv[])
{
    struct ovsdb_idl *idl;
    struct ctl_command *commands;
    struct shash local_options;
    unsigned int seqno;
    size_t n_commands;

    ovn_set_program_name(argv[0]);
    fatal_ignore_sigpipe();
    vlog_set_levels(NULL, VLF_CONSOLE, VLL_WARN);
    vlog_set_levels_from_string_assert("reconnect:warn");

    ic_sbctl_cmd_init();

    /* Parse command line. */
    char *args = process_escape_args(argv);
    shash_init(&local_options);
    parse_options(argc, argv, &local_options);
    char *error = ctl_parse_commands(argc - optind, argv + optind,
                                     &local_options, &commands, &n_commands);
    if (error) {
        ctl_fatal("%s", error);
    }
    VLOG(ctl_might_write_to_db(commands, n_commands) ? VLL_INFO : VLL_DBG,
         "Called as %s", args);

    ctl_timeout_setup(timeout);

    /* Initialize IDL. */
    idl = the_idl = ovsdb_idl_create_unconnected(&icsbrec_idl_class, true);
    ovsdb_idl_set_remote(idl, db, false);
    ovsdb_idl_set_db_change_aware(idl, false);
    ovsdb_idl_set_leader_only(idl, leader_only);

    /* Set reasonable high probe interval. */
    set_idl_probe_interval(idl, db, DEFAULT_UTILS_PROBE_INTERVAL_MSEC);

    run_prerequisites(commands, n_commands, idl);

    /* Execute the commands.
     *
     * 'seqno' is the database sequence number for which we last tried to
     * execute our transaction.  There's no point in trying to commit more than
     * once for any given sequence number, because if the transaction fails
     * it's because the database changed and we need to obtain an up-to-date
     * view of the database before we try the transaction again. */
    seqno = ovsdb_idl_get_seqno(idl);
    for (;;) {
        ovsdb_idl_run(idl);
        if (!ovsdb_idl_is_alive(idl)) {
            int retval = ovsdb_idl_get_last_error(idl);
            ctl_fatal("%s: database connection failed (%s)",
                        db, ovs_retval_to_string(retval));
        }

        if (seqno != ovsdb_idl_get_seqno(idl)) {
            seqno = ovsdb_idl_get_seqno(idl);
            if (do_ic_sbctl(args, commands, n_commands, idl)) {
                free(args);
                exit(EXIT_SUCCESS);
            }
        }

        if (seqno == ovsdb_idl_get_seqno(idl)) {
            ovsdb_idl_wait(idl);
            poll_block();
        }
    }
}

static void
parse_options(int argc, char *argv[], struct shash *local_options)
{
    enum {
        OPT_DB = UCHAR_MAX + 1,
        OPT_ONELINE,
        OPT_NO_SYSLOG,
        OPT_DRY_RUN,
        OPT_LOCAL,
        OPT_COMMANDS,
        OPT_OPTIONS,
        OPT_BOOTSTRAP_CA_CERT,
        VLOG_OPTION_ENUMS,
        TABLE_OPTION_ENUMS,
        SSL_OPTION_ENUMS,
    };
    static const struct option global_long_options[] = {
        {"db", required_argument, NULL, OPT_DB},
        {"no-syslog", no_argument, NULL, OPT_NO_SYSLOG},
        {"dry-run", no_argument, NULL, OPT_DRY_RUN},
        {"oneline", no_argument, NULL, OPT_ONELINE},
        {"timeout", required_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {"commands", no_argument, NULL, OPT_COMMANDS},
        {"options", no_argument, NULL, OPT_OPTIONS},
        {"leader-only", no_argument, &leader_only, true},
        {"no-leader-only", no_argument, &leader_only, false},
        {"version", no_argument, NULL, 'V'},
        VLOG_LONG_OPTIONS,
        STREAM_SSL_LONG_OPTIONS,
        {"bootstrap-ca-cert", required_argument, NULL, OPT_BOOTSTRAP_CA_CERT},
        TABLE_LONG_OPTIONS,
        {NULL, 0, NULL, 0},
    };
    const int n_global_long_options = ARRAY_SIZE(global_long_options) - 1;
    char *tmp, *short_options;

    struct option *options;
    size_t allocated_options;
    size_t n_options;
    size_t i;

    tmp = ovs_cmdl_long_options_to_short_options(global_long_options);
    short_options = xasprintf("+%s", tmp);
    free(tmp);

    /* We want to parse both global and command-specific options here, but
     * getopt_long() isn't too convenient for the job.  We copy our global
     * options into a dynamic array, then append all of the command-specific
     * options. */
    options = xmemdup(global_long_options, sizeof global_long_options);
    allocated_options = ARRAY_SIZE(global_long_options);
    n_options = n_global_long_options;
    ctl_add_cmd_options(&options, &n_options, &allocated_options, OPT_LOCAL);

    for (;;) {
        int idx;
        int c;

        c = getopt_long(argc, argv, short_options, options, &idx);
        if (c == -1) {
            break;
        }

        switch (c) {
        case OPT_DB:
            db = optarg;
            break;

        case OPT_ONELINE:
            oneline = true;
            break;

        case OPT_NO_SYSLOG:
            vlog_set_levels(&this_module, VLF_SYSLOG, VLL_WARN);
            break;

        case OPT_DRY_RUN:
            dry_run = true;
            break;

        case OPT_LOCAL:
            if (shash_find(local_options, options[idx].name)) {
                ctl_fatal("'%s' option specified multiple times",
                            options[idx].name);
            }
            shash_add_nocopy(local_options,
                             xasprintf("--%s", options[idx].name),
                             nullable_xstrdup(optarg));
            break;

        case 'h':
            usage();

        case OPT_COMMANDS:
            ctl_print_commands();
            /* fall through */

        case OPT_OPTIONS:
            ctl_print_options(global_long_options);
            /* fall through */

        case 'V':
            ovn_print_version(0, 0);
            printf("DB Schema %s\n", icsbrec_get_db_version());
            exit(EXIT_SUCCESS);

        case 't':
            if (!str_to_uint(optarg, 10, &timeout) || !timeout) {
                ctl_fatal("value %s on -t or --timeout is invalid", optarg);
            }
            break;

        VLOG_OPTION_HANDLERS
        TABLE_OPTION_HANDLERS(&table_style)
        STREAM_SSL_OPTION_HANDLERS

        case OPT_BOOTSTRAP_CA_CERT:
            stream_ssl_set_ca_cert_file(optarg, true);
            break;

        case '?':
            exit(EXIT_FAILURE);

        default:
            ovs_abort(0, "Internal error when parsing option %d.", c);

        case 0:
            break;
        }
    }
    free(short_options);

    if (!db) {
        db = default_ic_sb_db();
    }

    for (i = n_global_long_options; options[i].name; i++) {
        free(CONST_CAST(char *, options[i].name));
    }
    free(options);
}

static void
usage(void)
{
    printf("\
%s: OVN interconnection southbound DB management utility\n\
\n\
usage: %s [OPTIONS] COMMAND [ARG...]\n\
\n\
General commands:\n\
  show                        print overview of database contents\n\
\n\
Connection commands:\n\
  get-connection             print the connections\n\
  del-connection             delete the connections\n\
  [--inactivity-probe=MSECS]\n\
  set-connection TARGET...   set the list of connections to TARGET...\n\
\n\
SSL/TLS commands:\n\
  get-ssl                     print the SSL/TLS configuration\n\
  del-ssl                     delete the SSL/TLS configuration\n\
  set-ssl PRIV-KEY CERT CA-CERT [SSL-PROTOS [SSL-CIPHERS [SSL-CIPHERSUITES]]] \
set the SSL/TLS configuration\n\
\n\
%s\
%s\
\n\
Options:\n\
  --db=DATABASE               connect to DATABASE\n\
                              (default: %s)\n\
  --no-leader-only            accept any cluster member, not just the leader\n\
  -t, --timeout=SECS          wait at most SECS seconds\n\
  --dry-run                   do not commit changes to database\n\
  --oneline                   print exactly one line of output per command\n",
           program_name, program_name, ctl_get_db_cmd_usage(),
           ctl_list_db_tables_usage(), default_ic_sb_db());
    table_usage();
    vlog_usage();
    printf("\
  --no-syslog             equivalent to --verbose=ic_sbctl:syslog:warn\n");
    printf("\n\
Other options:\n\
  -h, --help                  display this help message\n\
  -V, --version               display version information\n");
    stream_usage("database", true, true, true);
    exit(EXIT_SUCCESS);
}


/* ovs-ic_sbctl specific context.  Inherits the 'struct ctl_context' as base.
 * Now empty, just keep the framework for future additions. */
struct ic_sbctl_context {
    struct ctl_context base;

    /* A cache of the contents of the database.
     *
     * A command that needs to use any of this information must first call
     * ic_sbctl_context_populate_cache().  A command that changes anything that
     * could invalidate the cache must either call
     * ic_sbctl_context_invalidate_cache() or manually update the cache to
     * maintain its correctness. */
    bool cache_valid;
};

static void
ic_sbctl_init(struct ctl_context *ctx OVS_UNUSED)
{
}

static char *
az_by_name_or_uuid(struct ctl_context *ctx, const char *id, bool must_exist,
                   const struct icsbrec_availability_zone **az_p)
{
    const struct icsbrec_availability_zone *az = NULL;
    *az_p = NULL;

    struct uuid az_uuid;
    bool is_uuid = uuid_from_string(&az_uuid, id);
    if (is_uuid) {
        az = icsbrec_availability_zone_get_for_uuid(ctx->idl, &az_uuid);
    }

    if (!az) {
        const struct icsbrec_availability_zone *iter;

        ICSBREC_AVAILABILITY_ZONE_FOR_EACH (iter, ctx->idl) {
            if (!strcmp(iter->name, id)) {
                az = iter;
                break;
            }
        }
    }

    if (!az && must_exist) {
        return xasprintf("%s: availability zone %s not found",
                         id, is_uuid ? "UUID" : "name");
    }

    *az_p = az;
    return NULL;
}

/* Data structures for the show command, which constructs the hierarchy
 * of availability_zone:gateway:port_binding. */
struct gw_data {
    const struct icsbrec_gateway *isb_gw;
    /* With node type struct icsbrec_port_binding.
     * We don't need to search port-binding, but using shash makes
     * sorting convenient. */
    struct shash pbs;
};

struct az_data {
    const struct icsbrec_availability_zone *isb_az;
    /* With node type struct gw_data */
    struct shash gws;
};

static void
init_az_info(struct ctl_context *ctx, struct shash *azs)
{
    const struct icsbrec_availability_zone *az;
    ICSBREC_AVAILABILITY_ZONE_FOR_EACH (az, ctx->idl) {
        struct az_data *az_data = xmalloc(sizeof *az_data);
        shash_init(&az_data->gws);
        az_data->isb_az = az;
        shash_add(azs, az->name, az_data);
    }

    const struct icsbrec_gateway *gw;
    ICSBREC_GATEWAY_FOR_EACH (gw, ctx->idl) {
        struct az_data *az_data = shash_find_data(azs,
                                                  gw->availability_zone->name);
        ovs_assert(az_data);
        struct gw_data *gw_data = xmalloc(sizeof *gw_data);
        gw_data->isb_gw = gw;
        shash_init(&gw_data->pbs);
        shash_add(&az_data->gws, gw->name, gw_data);
    }

    const struct icsbrec_port_binding *pb;
    ICSBREC_PORT_BINDING_FOR_EACH (pb, ctx->idl) {
        struct az_data *az_data = shash_find_data(azs,
                                                  pb->availability_zone->name);
        ovs_assert(az_data);
        struct gw_data *gw_data = shash_find_data(&az_data->gws, pb->gateway);
        if (!gw_data) {
            continue;
        }
        shash_add(&gw_data->pbs, pb->logical_port, pb);
    }
}

static void
destroy_az_info(struct shash *azs)
{
    struct shash_node *az_node;
    SHASH_FOR_EACH (az_node, azs) {
        struct az_data *az_data = az_node->data;
        struct shash_node *gw_node;
        SHASH_FOR_EACH (gw_node, &az_data->gws) {
            struct gw_data *gw_data = gw_node->data;
            shash_destroy(&gw_data->pbs);
            free(gw_data);
        }
        shash_destroy(&az_data->gws);
        free(az_data);
    }
    shash_destroy(azs);
}

static void
print_az(struct az_data *az, struct ds *s)
{
    ds_put_format(s, "availability-zone %s\n", az->isb_az->name);

    const struct shash_node **nodes = shash_sort(&az->gws);
    for (int g = 0; g < shash_count(&az->gws); g++) {
        struct gw_data *gw = nodes[g]->data;

        ds_put_format(s, "    gateway %s\n", gw->isb_gw->name);
        if (gw->isb_gw->hostname[0]) {
            ds_put_format(s, "        hostname: %s\n", gw->isb_gw->hostname);
        }

        for (int i = 0; i < gw->isb_gw->n_encaps; i++) {
            ds_put_format(s, "        type: %s\n",
                          gw->isb_gw->encaps[i]->type);
            ds_put_format(s, "            ip: %s\n",
                          gw->isb_gw->encaps[i]->ip);
        }

        const struct shash_node **pb_nodes = shash_sort(&gw->pbs);
        for (int p = 0; p < shash_count(&gw->pbs); p++) {
            const struct icsbrec_port_binding *pb = pb_nodes[p]->data;
            ds_put_format(s, "        port %s\n",
                          pb->logical_port);
            ds_put_format(s, "            transit switch: %s\n",
                          pb->transit_switch);
            ds_put_format(s, "            address: [\"%s\"]\n",
                          pb->address);
        }
        free(pb_nodes);
    }
    free(nodes);
}

static void
ic_sbctl_show(struct ctl_context *ctx)
{
    /* Availability zones with the gateways and port-bindings data. Node
     * type is struct az_data. */
    struct shash azs = SHASH_INITIALIZER(&azs);
    init_az_info(ctx, &azs);

    const struct icsbrec_availability_zone *az;

    if (ctx->argc == 2) {
        char *error = az_by_name_or_uuid(ctx, ctx->argv[1], false, &az);
        if (error) {
            ctx->error = error;
            goto error;
        }
        if (az) {
            struct az_data *az_data = shash_find_data(&azs, az->name);
            ovs_assert(az_data);
            print_az(az_data, &ctx->output);
        }
    } else {
        const struct shash_node **nodes = shash_sort(&azs);
        for (int i = 0; i < shash_count(&azs); i++) {
            struct az_data *az_data = nodes[i]->data;
            print_az(az_data, &ctx->output);
        }
        free(nodes);
    }

error:
    destroy_az_info(&azs);
}

static void
verify_connections(struct ctl_context *ctx)
{
    const struct icsbrec_ic_sb_global *ic_sb_global =
        icsbrec_ic_sb_global_first(ctx->idl);
    const struct icsbrec_connection *conn;

    icsbrec_ic_sb_global_verify_connections(ic_sb_global);

    ICSBREC_CONNECTION_FOR_EACH (conn, ctx->idl) {
        icsbrec_connection_verify_target(conn);
    }
}

static void
pre_connection(struct ctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &icsbrec_ic_sb_global_col_connections);
    ovsdb_idl_add_column(ctx->idl, &icsbrec_connection_col_target);
    ovsdb_idl_add_column(ctx->idl, &icsbrec_connection_col_inactivity_probe);
}

static void
cmd_get_connection(struct ctl_context *ctx)
{
    const struct icsbrec_connection *conn;
    struct svec targets;
    size_t i;

    verify_connections(ctx);

    /* Print the targets in sorted order for reproducibility. */
    svec_init(&targets);

    ICSBREC_CONNECTION_FOR_EACH (conn, ctx->idl) {
        svec_add(&targets, conn->target);
    }

    svec_sort_unique(&targets);
    for (i = 0; i < targets.n; i++) {
        ds_put_format(&ctx->output, "%s\n", targets.names[i]);
    }
    svec_destroy(&targets);
}

static void
delete_connections(struct ctl_context *ctx)
{
    const struct icsbrec_ic_sb_global *ic_sb_global =
        icsbrec_ic_sb_global_first(ctx->idl);
    const struct icsbrec_connection *conn;

    /* Delete Manager rows pointed to by 'connection_options' column. */
    ICSBREC_CONNECTION_FOR_EACH_SAFE (conn, ctx->idl) {
        icsbrec_connection_delete(conn);
    }

    /* Delete 'Manager' row refs in 'manager_options' column. */
    icsbrec_ic_sb_global_set_connections(ic_sb_global, NULL, 0);
}

static void
cmd_del_connection(struct ctl_context *ctx)
{
    verify_connections(ctx);
    delete_connections(ctx);
}

static void
insert_connections(struct ctl_context *ctx, char *targets[], size_t n)
{
    const struct icsbrec_ic_sb_global *ic_sb_global =
        icsbrec_ic_sb_global_first(ctx->idl);
    struct icsbrec_connection **connections;
    size_t i, conns = 0;
    const char *inactivity_probe = shash_find_data(&ctx->options,
                                                   "--inactivity-probe");

    /* Insert each connection in a new row in Connection table. */
    connections = xmalloc(n * sizeof *connections);
    for (i = 0; i < n; i++) {
        if (stream_verify_name(targets[i]) &&
                   pstream_verify_name(targets[i])) {
            VLOG_WARN("target type \"%s\" is possibly erroneous", targets[i]);
        }

        connections[conns] = icsbrec_connection_insert(ctx->txn);
        icsbrec_connection_set_target(connections[conns], targets[i]);
        if (inactivity_probe) {
            int64_t msecs = atoll(inactivity_probe);
            icsbrec_connection_set_inactivity_probe(connections[conns],
                                                  &msecs, 1);
        }
        conns++;
    }

    /* Store uuids of new connection rows in 'connection' column. */
    icsbrec_ic_sb_global_set_connections(ic_sb_global, connections, conns);
    free(connections);
}

static void
cmd_set_connection(struct ctl_context *ctx)
{
    const size_t n = ctx->argc - 1;

    verify_connections(ctx);
    delete_connections(ctx);
    insert_connections(ctx, &ctx->argv[1], n);
}

static void
pre_cmd_get_ssl(struct ctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &icsbrec_ic_sb_global_col_ssl);

    ovsdb_idl_add_column(ctx->idl, &icsbrec_ssl_col_private_key);
    ovsdb_idl_add_column(ctx->idl, &icsbrec_ssl_col_certificate);
    ovsdb_idl_add_column(ctx->idl, &icsbrec_ssl_col_ca_cert);
    ovsdb_idl_add_column(ctx->idl, &icsbrec_ssl_col_bootstrap_ca_cert);
}

static void
cmd_get_ssl(struct ctl_context *ctx)
{
    const struct icsbrec_ic_sb_global *ic_sb_global =
        icsbrec_ic_sb_global_first(ctx->idl);
    const struct icsbrec_ssl *ssl = icsbrec_ssl_first(ctx->idl);

    icsbrec_ic_sb_global_verify_ssl(ic_sb_global);
    if (ssl) {
        icsbrec_ssl_verify_private_key(ssl);
        icsbrec_ssl_verify_certificate(ssl);
        icsbrec_ssl_verify_ca_cert(ssl);
        icsbrec_ssl_verify_bootstrap_ca_cert(ssl);

        ds_put_format(&ctx->output, "Private key: %s\n", ssl->private_key);
        ds_put_format(&ctx->output, "Certificate: %s\n", ssl->certificate);
        ds_put_format(&ctx->output, "CA Certificate: %s\n", ssl->ca_cert);
        ds_put_format(&ctx->output, "Bootstrap: %s\n",
                ssl->bootstrap_ca_cert ? "true" : "false");
    }
}

static void
pre_cmd_del_ssl(struct ctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &icsbrec_ic_sb_global_col_ssl);
}

static void
cmd_del_ssl(struct ctl_context *ctx)
{
    const struct icsbrec_ic_sb_global *ic_sb_global =
        icsbrec_ic_sb_global_first(ctx->idl);
    const struct icsbrec_ssl *ssl = icsbrec_ssl_first(ctx->idl);

    if (ssl) {
        icsbrec_ic_sb_global_verify_ssl(ic_sb_global);
        icsbrec_ssl_delete(ssl);
        icsbrec_ic_sb_global_set_ssl(ic_sb_global, NULL);
    }
}

static void
pre_cmd_set_ssl(struct ctl_context *ctx)
{
    ovsdb_idl_add_column(ctx->idl, &icsbrec_ic_sb_global_col_ssl);
}

static void
cmd_set_ssl(struct ctl_context *ctx)
{
    bool bootstrap = shash_find(&ctx->options, "--bootstrap");
    const struct icsbrec_ic_sb_global *ic_sb_global =
        icsbrec_ic_sb_global_first(ctx->idl);
    const struct icsbrec_ssl *ssl = icsbrec_ssl_first(ctx->idl);

    icsbrec_ic_sb_global_verify_ssl(ic_sb_global);
    if (ssl) {
        icsbrec_ssl_delete(ssl);
    }
    ssl = icsbrec_ssl_insert(ctx->txn);

    icsbrec_ssl_set_private_key(ssl, ctx->argv[1]);
    icsbrec_ssl_set_certificate(ssl, ctx->argv[2]);
    icsbrec_ssl_set_ca_cert(ssl, ctx->argv[3]);

    icsbrec_ssl_set_bootstrap_ca_cert(ssl, bootstrap);

    if (ctx->argc >= 5) {
        icsbrec_ssl_set_ssl_protocols(ssl, ctx->argv[4]);
        if (ctx->argc >= 6) {
            icsbrec_ssl_set_ssl_ciphers(ssl, ctx->argv[5]);
            if (ctx->argc == 7) {
                icsbrec_ssl_set_ssl_ciphersuites(ssl, ctx->argv[6]);
            }
        }
    }

    icsbrec_ic_sb_global_set_ssl(ic_sb_global, ssl);
}


static const struct ctl_table_class tables[ICSBREC_N_TABLES] = {
    [ICSBREC_TABLE_AVAILABILITY_ZONE].row_ids[0] =
    {&icsbrec_availability_zone_col_name, NULL, NULL},

    [ICSBREC_TABLE_GATEWAY].row_ids[0] =
    {&icsbrec_gateway_col_name, NULL, NULL},

    [ICSBREC_TABLE_PORT_BINDING].row_ids[0] =
    {&icsbrec_port_binding_col_logical_port, NULL, NULL},

    [ICSBREC_TABLE_DATAPATH_BINDING].row_ids[0] =
    {&icsbrec_datapath_binding_col_transit_switch, NULL, NULL},
};


static void
ic_sbctl_context_init_command(struct ic_sbctl_context *ic_sbctl_ctx,
                           struct ctl_command *command, bool last_command)
{
    ctl_context_init_command(&ic_sbctl_ctx->base, command, last_command);
}

static void
ic_sbctl_context_init(struct ic_sbctl_context *ic_sbctl_ctx,
                   struct ctl_command *command, struct ovsdb_idl *idl,
                   struct ovsdb_idl_txn *txn,
                   struct ovsdb_symbol_table *symtab)
{
    ctl_context_init(&ic_sbctl_ctx->base, command, idl, txn, symtab, NULL);
    ic_sbctl_ctx->cache_valid = false;
}

static void
ic_sbctl_context_done_command(struct ic_sbctl_context *ic_sbctl_ctx,
                           struct ctl_command *command)
{
    ctl_context_done_command(&ic_sbctl_ctx->base, command);
}

static void
ic_sbctl_context_done(struct ic_sbctl_context *ic_sbctl_ctx,
                   struct ctl_command *command)
{
    ctl_context_done(&ic_sbctl_ctx->base, command);
}

static void
run_prerequisites(struct ctl_command *commands, size_t n_commands,
                  struct ovsdb_idl *idl)
{
    ovsdb_idl_add_table(idl, &icsbrec_table_ic_sb_global);

    for (struct ctl_command *c = commands; c < &commands[n_commands]; c++) {
        if (c->syntax->prerequisites) {
            struct ic_sbctl_context ic_sbctl_ctx;

            ds_init(&c->output);
            c->table = NULL;

            ic_sbctl_context_init(&ic_sbctl_ctx, c, idl, NULL, NULL);
            (c->syntax->prerequisites)(&ic_sbctl_ctx.base);
            if (ic_sbctl_ctx.base.error) {
                ctl_fatal("%s", ic_sbctl_ctx.base.error);
            }
            ic_sbctl_context_done(&ic_sbctl_ctx, c);

            ovs_assert(!c->output.string);
            ovs_assert(!c->table);
        }
    }
}

static bool
do_ic_sbctl(const char *args, struct ctl_command *commands, size_t n_commands,
         struct ovsdb_idl *idl)
{
    struct ovsdb_idl_txn *txn;
    enum ovsdb_idl_txn_status status;
    struct ovsdb_symbol_table *symtab;
    struct ic_sbctl_context ic_sbctl_ctx;
    struct ctl_command *c;
    struct shash_node *node;

    txn = the_idl_txn = ovsdb_idl_txn_create(idl);
    if (dry_run) {
        ovsdb_idl_txn_set_dry_run(txn);
    }

    ovsdb_idl_txn_add_comment(txn, "ovs-ic_sbctl: %s", args);

    const struct icsbrec_ic_sb_global *isb = icsbrec_ic_sb_global_first(idl);
    if (!isb) {
        /* XXX add verification that table is empty */
        icsbrec_ic_sb_global_insert(txn);
    }

    symtab = ovsdb_symbol_table_create();
    for (c = commands; c < &commands[n_commands]; c++) {
        ds_init(&c->output);
        c->table = NULL;
    }
    ic_sbctl_context_init(&ic_sbctl_ctx, NULL, idl, txn, symtab);
    for (c = commands; c < &commands[n_commands]; c++) {
        ic_sbctl_context_init_command(&ic_sbctl_ctx, c,
                                      c == &commands[n_commands - 1]);
        if (c->syntax->run) {
            (c->syntax->run)(&ic_sbctl_ctx.base);
        }
        if (ic_sbctl_ctx.base.error) {
            ctl_fatal("%s", ic_sbctl_ctx.base.error);
        }
        ic_sbctl_context_done_command(&ic_sbctl_ctx, c);

        if (ic_sbctl_ctx.base.try_again) {
            ic_sbctl_context_done(&ic_sbctl_ctx, NULL);
            goto try_again;
        }
    }
    ic_sbctl_context_done(&ic_sbctl_ctx, NULL);

    SHASH_FOR_EACH (node, &symtab->sh) {
        struct ovsdb_symbol *symbol = node->data;
        if (!symbol->created) {
            ctl_fatal("row id \"%s\" is referenced but never created (e.g. "
                      "with \"-- --id=%s create ...\")",
                      node->name, node->name);
        }
        if (!symbol->strong_ref) {
            if (!symbol->weak_ref) {
                VLOG_WARN("row id \"%s\" was created but no reference to it "
                          "was inserted, so it will not actually appear in "
                          "the database", node->name);
            } else {
                VLOG_WARN("row id \"%s\" was created but only a weak "
                          "reference to it was inserted, so it will not "
                          "actually appear in the database", node->name);
            }
        }
    }

    status = ovsdb_idl_txn_commit_block(txn);
    if (status == TXN_UNCHANGED || status == TXN_SUCCESS) {
        for (c = commands; c < &commands[n_commands]; c++) {
            if (c->syntax->postprocess) {
                ic_sbctl_context_init(&ic_sbctl_ctx, c, idl, txn, symtab);
                (c->syntax->postprocess)(&ic_sbctl_ctx.base);
                if (ic_sbctl_ctx.base.error) {
                    ctl_fatal("%s", ic_sbctl_ctx.base.error);
                }
                ic_sbctl_context_done(&ic_sbctl_ctx, c);
            }
        }
    }

    switch (status) {
    case TXN_UNCOMMITTED:
    case TXN_INCOMPLETE:
        OVS_NOT_REACHED();

    case TXN_ABORTED:
        /* Should not happen--we never call ovsdb_idl_txn_abort(). */
        ctl_fatal("transaction aborted");

    case TXN_UNCHANGED:
    case TXN_SUCCESS:
        break;

    case TXN_TRY_AGAIN:
        goto try_again;

    case TXN_ERROR:
        ctl_fatal("transaction error: %s", ovsdb_idl_txn_get_error(txn));

    case TXN_NOT_LOCKED:
        /* Should not happen--we never call ovsdb_idl_set_lock(). */
        ctl_fatal("database not locked");

    default:
        OVS_NOT_REACHED();
    }

    ovsdb_symbol_table_destroy(symtab);

    for (c = commands; c < &commands[n_commands]; c++) {
        struct ds *ds = &c->output;

        if (c->table) {
            table_print(c->table, &table_style);
        } else if (oneline) {
            size_t j;

            ds_chomp(ds, '\n');
            for (j = 0; j < ds->length; j++) {
                int ch = ds->string[j];
                switch (ch) {
                case '\n':
                    fputs("\\n", stdout);
                    break;

                case '\\':
                    fputs("\\\\", stdout);
                    break;

                default:
                    putchar(ch);
                }
            }
            putchar('\n');
        } else {
            fputs(ds_cstr(ds), stdout);
        }
        ds_destroy(&c->output);
        table_destroy(c->table);
        free(c->table);

        shash_destroy_free_data(&c->options);
    }
    free(commands);
    ovsdb_idl_txn_destroy(txn);
    ovsdb_idl_destroy(idl);

    return true;

try_again:
    /* Our transaction needs to be rerun, or a prerequisite was not met.  Free
     * resources and return so that the caller can try again. */
    ovsdb_idl_txn_abort(txn);
    ovsdb_idl_txn_destroy(txn);
    the_idl_txn = NULL;

    ovsdb_symbol_table_destroy(symtab);
    for (c = commands; c < &commands[n_commands]; c++) {
        ds_destroy(&c->output);
        table_destroy(c->table);
        free(c->table);
    }
    return false;
}

/* Frees the current transaction and the underlying IDL and then calls
 * exit(status).
 *
 * Freeing the transaction and the IDL is not strictly necessary, but it makes
 * for a clean memory leak report from valgrind in the normal case.  That makes
 * it easier to notice real memory leaks. */
static void
ic_sbctl_exit(int status)
{
    if (the_idl_txn) {
        ovsdb_idl_txn_abort(the_idl_txn);
        ovsdb_idl_txn_destroy(the_idl_txn);
    }
    ovsdb_idl_destroy(the_idl);
    exit(status);
}

static const struct ctl_command_syntax ic_sbctl_commands[] = {
    { "init", 0, 0, "", NULL, ic_sbctl_init, NULL, "", RW },
    { "show", 0, 1, "[AZ]", NULL, ic_sbctl_show, NULL, "", RO },

    /* Connection commands. */
    {"get-connection", 0, 0, "", pre_connection, cmd_get_connection, NULL, "",
        RO},
    {"del-connection", 0, 0, "", pre_connection, cmd_del_connection, NULL, "",
        RW},
    {"set-connection", 1, INT_MAX, "TARGET...", pre_connection,
        cmd_set_connection, NULL, "--inactivity-probe=", RW},

    /* SSL/TLS commands. */
    {"get-ssl", 0, 0, "", pre_cmd_get_ssl, cmd_get_ssl, NULL, "", RO},
    {"del-ssl", 0, 0, "", pre_cmd_del_ssl, cmd_del_ssl, NULL, "", RW},
    {"set-ssl", 3, 6,
        "PRIVATE-KEY CERTIFICATE CA-CERT"
        " [SSL-PROTOS [SSL-CIPHERS [SSL-CIPHERSUITES]]]",
        pre_cmd_set_ssl, cmd_set_ssl, NULL, "--bootstrap", RW},

    {NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, RO},
};

/* Registers ic_sbctl and common db commands. */
static void
ic_sbctl_cmd_init(void)
{
    ctl_init(&icsbrec_idl_class, icsbrec_table_classes, tables,
             NULL, ic_sbctl_exit);
    ctl_register_commands(ic_sbctl_commands);
}
