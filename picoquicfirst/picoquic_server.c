/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WINDOWS
#define WIN32_LEAN_AND_MEAN
#include "getopt.h"
#include <WinSock2.h>
#include <Windows.h>

#define SERVER_CERT_FILE "certs\\cert.pem"
#define SERVER_KEY_FILE  "certs\\key.pem"

#else /* Linux */

#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#ifndef __USE_XOPEN2K
#define __USE_XOPEN2K
#endif
#ifndef __USE_POSIX
#define __USE_POSIX
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>

#define SERVER_CERT_FILE "certs/cert.pem"
#define SERVER_KEY_FILE "certs/key.pem"

#endif

static const int default_server_port = 4443;
static const char* default_server_name = "::";
static const char* ticket_store_filename = "demo_ticket_store.bin";
static const char* token_store_filename = "demo_token_store.bin";


#include "picoquic.h"
#include "picoquic_packet_loop.h"
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "autoqlog.h"
#include "h3zero.h"
#include "h3zero_common.h"
#include "pico_webtransport.h"
#include "wt_baton.h"
#include "democlient.h"
#include "demoserver.h"
#include "siduck.h"
#include "quicperf.h"
#include "picoquic_unified_log.h"
#include "picoquic_logger.h"
#include "picoquic_binlog.h"
#include "performance_log.h"
#include "picoquic_config.h"
#include "picoquic_lb.h"

/*
 * SIDUCK datagram demo call back.
 */
int siduck_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx);

void print_address(FILE* F_log, struct sockaddr* address, char* label, picoquic_connection_id_t cnx_id)
{
    char hostname[256];

    const char* x = inet_ntop(address->sa_family,
        (address->sa_family == AF_INET) ? (void*)&(((struct sockaddr_in*)address)->sin_addr) : (void*)&(((struct sockaddr_in6*)address)->sin6_addr),
        hostname, sizeof(hostname));

    fprintf(F_log, "%016llx : ", (unsigned long long)picoquic_val64_connection_id(cnx_id));

    if (x != NULL) {
        fprintf(F_log, "%s %s, port %d\n", label, x,
            (address->sa_family == AF_INET) ? ((struct sockaddr_in*)address)->sin_port : ((struct sockaddr_in6*)address)->sin6_port);
    } else {
        fprintf(F_log, "%s: inet_ntop failed with error # %ld\n", label, WSA_LAST_ERROR(errno));
    }
}

/* server loop call back management */
typedef struct st_server_loop_cb_t {
    int just_once;
    int first_connection_seen;
    int connection_done;
} server_loop_cb_t;

static int server_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode,
    void* callback_ctx, void * callback_arg)
{
    int ret = 0;
    server_loop_cb_t* cb_ctx = (server_loop_cb_t*)callback_ctx;

    if (cb_ctx == NULL) {
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    else {
        switch (cb_mode) {
        case picoquic_packet_loop_ready:
            fprintf(stdout, "Waiting for packets.\n");
            break;
        case picoquic_packet_loop_after_receive:
            break;
        case picoquic_packet_loop_after_send:
            break;
        case picoquic_packet_loop_port_update:
            break;
        default:
            ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
            break;
        }

        if (ret == 0 && cb_ctx->just_once){
            if (!cb_ctx->first_connection_seen && picoquic_get_first_cnx(quic) != NULL) {
                cb_ctx->first_connection_seen = 1;
                fprintf(stdout, "First connection noticed.\n");
            } else if (cb_ctx->first_connection_seen && picoquic_get_first_cnx(quic) == NULL) {
                fprintf(stdout, "No more active connections.\n");
                cb_ctx->connection_done = 1;
                ret = PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }
        }
    }
    return ret;
}

/* Define a "post" context used to test the "post" function "end to end".
 */
typedef struct st_demoserver_post_test_t {
    size_t nb_received;
    size_t nb_sent;
    size_t response_length;
    char posted[256];
} demoserver_post_test_t;

int demoserver_post_callback(picoquic_cnx_t* cnx,
    uint8_t* bytes, size_t length,
    picohttp_call_back_event_t event, h3zero_stream_ctx_t* stream_ctx,
    void * callback_ctx)
{
    int ret = 0;
    demoserver_post_test_t* ctx = (demoserver_post_test_t*)stream_ctx->path_callback_ctx;

    switch (event) {
    case picohttp_callback_get: /* Received a get command */
        break;
    case picohttp_callback_post: /* Received a post command */
        if (ctx == NULL) {
            ctx = (demoserver_post_test_t*)malloc(sizeof(demoserver_post_test_t));
            if (ctx == NULL) {
                /* cannot handle the stream -- TODO: reset stream? */
                return -1;
            }
            else {
                memset(ctx, 0, sizeof(demoserver_post_test_t));
                stream_ctx->path_callback_ctx = (void*)ctx;
            }
        }
        else {
            /* unexpected. Should not have a context here */
            return -1;
        }
        break;
    case picohttp_callback_post_data: /* Data received from peer on stream N */
    case picohttp_callback_post_fin: /* All posted data have been received, prepare the response now. */
        /* Add data to echo size */
        if (ctx == NULL) {
            ret = -1;
        }
        else {
            ctx->nb_received += length;
            if (event == picohttp_callback_post_fin) {
                if (ctx != NULL) {
                    size_t nb_chars = 0;
                    if (picoquic_sprintf(ctx->posted, sizeof(ctx->posted), &nb_chars, "Received %zu bytes.\n", ctx->nb_received) >= 0) {
                        ctx->response_length = nb_chars;
                        ret = (int)nb_chars;
                        if (ctx->response_length <= length) {
                            memcpy(bytes, ctx->posted, ctx->response_length);
                        }
                    }
                    else {
                        ret = -1;
                    }
                }
                else {
                    ret = -1;
                }
            }
        }
        break;
    case picohttp_callback_provide_data:
        if (ctx == NULL || ctx->nb_sent > ctx->response_length) {
            ret = -1;
        }
        else
        {
            /* Provide data. */
            uint8_t* buffer;
            size_t available = ctx->response_length - ctx->nb_sent;
            int is_fin = 1;

            if (available > length) {
                available = length;
                is_fin = 0;
            }

            buffer = picoquic_provide_stream_data_buffer(bytes, available, is_fin, !is_fin);
            if (buffer != NULL) {
                memcpy(buffer, ctx->posted + ctx->nb_sent, available);
                ctx->nb_sent += available;
                ret = 0;
            }
            else {
                ret = -1;
            }
        }
        break;
    case picohttp_callback_reset: /* stream is abandoned */
        stream_ctx->path_callback = NULL;
        stream_ctx->path_callback_ctx = NULL;

        if (ctx != NULL) {
            free(ctx);
        }
        break;
    default:
        ret = -1;
        break;
    }

    return ret;
}

picohttp_server_path_item_t path_item_list[2] =
{
    {
        "/post",
        5,
        demoserver_post_callback,
        NULL
    },
    {
        "/baton",
        6,
        wt_baton_callback,
        NULL
    }
};

int quic_server(const char* server_name, picoquic_quic_config_t * config, int just_once)
{
    /* Start: start the QUIC process with cert and key files */
    int ret = 0;
    picoquic_quic_t* qserver = NULL;
    uint64_t current_time = 0;
    picohttp_server_parameters_t picoquic_file_param;
    server_loop_cb_t loop_cb_ctx;

    memset(&picoquic_file_param, 0, sizeof(picohttp_server_parameters_t));
    picoquic_file_param.web_folder = config->www_dir;
    picoquic_file_param.path_table = path_item_list;
    picoquic_file_param.path_table_nb = 2;

    memset(&loop_cb_ctx, 0, sizeof(server_loop_cb_t));
    loop_cb_ctx.just_once = just_once;

    /* Setup the server context */
    if (ret == 0) {
        current_time = picoquic_current_time();
        /* Create QUIC context */

        if (config->ticket_file_name == NULL) {
            ret = picoquic_config_set_option(config, picoquic_option_Ticket_File_Name, ticket_store_filename);
        }
        if (ret == 0 && config->token_file_name == NULL) {
            ret = picoquic_config_set_option(config, picoquic_option_Token_File_Name, token_store_filename);
        }
        if (ret == 0) {
            qserver = picoquic_create_and_configure(config, picoquic_demo_server_callback, &picoquic_file_param, current_time, NULL);
            if (qserver == NULL) {
                ret = -1;
            }
            else {
                picoquic_set_key_log_file_from_env(qserver);

                picoquic_set_alpn_select_fn(qserver, picoquic_demo_server_callback_select_alpn);

                picoquic_use_unique_log_names(qserver, 1);

                picoquic_set_default_multipath_option(qserver, 1);  // Enable multipath
                picoquic_enable_path_callbacks_default(qserver, 1); // Enable path callbacks
                picoquic_set_default_congestion_algorithm(qserver, picoquic_cubic_algorithm); // Set to cubic

                if (config->qlog_dir != NULL)
                {
                    picoquic_set_qlog(qserver, config->qlog_dir);
                }
                if (config->performance_log != NULL)
                {
                    ret = picoquic_perflog_setup(qserver, config->performance_log);
                }
                if (ret == 0 && config->cnx_id_cbdata != NULL) {
                    picoquic_load_balancer_config_t lb_config;
                    ret = picoquic_lb_compat_cid_config_parse(&lb_config, config->cnx_id_cbdata, strlen(config->cnx_id_cbdata));
                    if (ret != 0) {
                        fprintf(stdout, "Cannot parse the CNX_ID config policy: %s.\n", config->cnx_id_cbdata);
                    }
                    else {
                        ret = picoquic_lb_compat_cid_config(qserver, &lb_config);
                        if (ret != 0) {
                            fprintf(stdout, "Cannot set the CNX_ID config policy: %s.\n", config->cnx_id_cbdata);
                        }
                    }
                }
                if (ret == 0) {
                    fprintf(stdout, "Accept enable multipath: %d.\n", qserver->default_multipath_option);
                }
            }
        }
    }

    if (ret == 0) {
        /* Wait for packets */
#if _WINDOWS_BUT_WE_ARE_UNIFYING
        ret = picoquic_packet_loop_win(qserver, config->server_port, 0, config->dest_if, 
            config->socket_buffer_size, server_loop_cb, &loop_cb_ctx);
#else
        ret = picoquic_packet_loop(qserver, config->server_port, 0, config->dest_if,
            config->socket_buffer_size, config->do_not_use_gso, server_loop_cb, &loop_cb_ctx);
#endif
    }

    /* And exit */
    printf("Server exit, ret = 0x%x\n", ret);

    /* Clean up */
    if (config->cnx_id_cbdata != NULL) {
        picoquic_lb_compat_cid_config_free(qserver);
    }
    if (qserver != NULL) {
        picoquic_free(qserver);
    }

    return ret;
}

/* TODO: rewrite using common code */
void usage()
{
    fprintf(stderr, "PicoQUIC demo client and server\n");
    fprintf(stderr, "Usage: picoquicdemo <options> [server_name [port [scenario]]] \n");
    fprintf(stderr, "  For the client mode, specify server_name and port.\n");
    fprintf(stderr, "  For the server mode, use -p to specify the port.\n");
    picoquic_config_usage();
    fprintf(stderr, "Picoquic demo options:\n");
    fprintf(stderr, "  -A \"ip/ifindex[,ip/ifindex]\"  IP and interface index for multipath alternative\n");
    fprintf(stderr, "                        path, e.g. \"10.0.0.2/3,10.0.0.3/4\". This option only\n");
    fprintf(stderr, "                        affects the behavior of the client.\n");
    fprintf(stderr, "  -f migration_mode     Force client to migrate to start migration:\n");
    fprintf(stderr, "                        -f 1  test NAT rebinding,\n");
    fprintf(stderr, "                        -f 2  test CNXID renewal,\n");
    fprintf(stderr, "                        -f 3  test migration to new address.\n");
    fprintf(stderr, "  -u nb                 trigger key update after receiving <nb> packets on client\n");
    fprintf(stderr, "  -1                    Once: close the server after processing 1 connection.\n");

    fprintf(stderr, "\nThe scenario argument specifies the set of files that should be retrieved,\n");
    fprintf(stderr, "and their order. The syntax is:\n");
    fprintf(stderr, "  *{[<stream_id>':'[<previous_stream>':'[<format>:]]]path;}\n");
    fprintf(stderr, "where:\n");
    fprintf(stderr, "  <stream_id>:          The numeric ID of the QUIC stream, e.g. 4. By default, the\n");
    fprintf(stderr, "                        next stream in the logical QUIC order, 0, 4, 8, etc.");
    fprintf(stderr, "  <previous_stream>:    The numeric ID of the previous stream. The GET command will\n");
    fprintf(stderr, "                        be issued after that stream's transfer finishes. By default,\n");
    fprintf(stderr, "                        previous stream in this scenario.\n");
    fprintf(stderr, "  <format>:             Whether the received file should be written to disc as\n");
    fprintf(stderr, "                        binary(b) or text(t). Defaults to text.\n");
    fprintf(stderr, "  <path>:               The name of the document that should be retrieved\n");
    fprintf(stderr, "If no scenario is specified, the client executes the default scenario.\n");

    exit(1);
}

int main(int argc, char** argv)
{
    picoquic_quic_config_t config;
    char option_string[512];
    int opt;
    const char* server_name = default_server_name;
    int server_port = default_server_port;
    char default_server_cert_file[512];
    char default_server_key_file[512];
    char* client_scenario = NULL;
    int nb_packets_before_update = 0;
    int force_migration = 0;
    int just_once = 0;
    int is_client = 0;
    int ret;

#ifdef _WINDOWS
    WSADATA wsaData = { 0 };
    (void)WSA_START(MAKEWORD(2, 2), &wsaData);
#endif
    picoquic_config_init(&config);
    memcpy(option_string, "A:u:f:1", 7);
    ret = picoquic_config_option_letters(option_string + 7, sizeof(option_string) - 7, NULL);

    if (ret == 0) {
        /* Get the parameters */
        while ((opt = getopt(argc, argv, option_string)) != -1) {
            switch (opt) {
            case 'u':
                if ((nb_packets_before_update = atoi(optarg)) <= 0) {
                    fprintf(stderr, "Invalid number of packets: %s\n", optarg);
                    usage();
                }
                break;
            case 'f':
                force_migration = atoi(optarg);
                if (force_migration <= 0 || force_migration > 3) {
                    fprintf(stderr, "Invalid migration mode: %s\n", optarg);
                    usage();
                }
                break;
            case '1':
                just_once = 1;
                break;
            case 'A':
                config.multipath_alt_config = malloc(sizeof(char) * (strlen(optarg) + 1));
                memcpy(config.multipath_alt_config, optarg, sizeof(char) * (strlen(optarg) + 1));
                printf("config.multipath_alt_config: %s\n", config.multipath_alt_config);
                break;
            default:
                if (picoquic_config_command_line(opt, &optind, argc, (char const **)argv, optarg, &config) != 0) {
                    usage();
                }
                break;
            }
        }
    }

    /* Simplified style params */
    if (optind < argc) {
        server_name = argv[optind++];
        is_client = 1;
    }

    if (optind < argc) {
        if ((server_port = atoi(argv[optind++])) <= 0) {
            fprintf(stderr, "Invalid port: %s\n", optarg);
            usage();
        }
    }

    if (optind < argc) {
        client_scenario = argv[optind++];
    }

    if (optind < argc) {
        usage();
    }

    if (config.server_port == 0) {
        config.server_port = server_port;
    }

    if (config.server_cert_file == NULL &&
        picoquic_get_input_path(default_server_cert_file, sizeof(default_server_cert_file), config.solution_dir, SERVER_CERT_FILE) == 0) {
        /* Using set option call to ensure proper memory management*/
        picoquic_config_set_option(&config, picoquic_option_CERT, default_server_cert_file);
    }

    if (config.server_key_file == NULL &&
        picoquic_get_input_path(default_server_key_file, sizeof(default_server_key_file), config.solution_dir, SERVER_KEY_FILE) == 0) {
        /* Using set option call to ensure proper memory management*/
        picoquic_config_set_option(&config, picoquic_option_KEY, default_server_key_file);
    }

    /* Run as server */
    printf("Starting Picoquic server (v%s) on port %d, server name = %s, just_once = %d, do_retry = %d\n",
        PICOQUIC_VERSION, config.server_port, server_name, just_once, config.do_retry);
    
    printf("WWW dir = %s\n", config.www_dir);

    ret = quic_server(server_name, &config, just_once);
    printf("Server exit with code = %d\n", ret);

    picoquic_config_clear(&config);
}
