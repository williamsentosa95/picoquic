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


#include <iostream>
#include <netinet/in.h>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>

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
#include <cassert>
#include <map>
#include <queue>
#include <mutex>

#define STREAM_ID_INITIAL UINT64_MAX

using namespace std;

static const char* ticket_store_filename = "demo_ticket_store.bin";
static const char* token_store_filename = "demo_token_store.bin";

static const char* request_filesize_delimitter = "fszb-";

static picoquic_alpn_list_t alpn_list[] = {
    { picoquic_alpn_http_3, "h3", 2 },
    { picoquic_alpn_http_0_9, "hq-interop", 10 },
    { picoquic_alpn_http_3, "h3-34", 5 },
    { picoquic_alpn_http_0_9, "hq-34", 5 },
    { picoquic_alpn_http_3, "h3-33", 5 },
    { picoquic_alpn_http_0_9, "hq-33", 5 },
    { picoquic_alpn_http_3, "h3-32", 5 },
    { picoquic_alpn_http_0_9, "hq-32", 5 },
    { picoquic_alpn_http_3, "h3-31", 5 },
    { picoquic_alpn_http_0_9, "hq-31", 5 },
    { picoquic_alpn_http_3, "h3-29", 5 },
    { picoquic_alpn_http_0_9, "hq-29", 5 },
    { picoquic_alpn_http_3, "h3-30", 5 },
    { picoquic_alpn_http_0_9, "hq-30", 5 },
    { picoquic_alpn_http_3, "h3-28", 5 },
    { picoquic_alpn_http_0_9, "hq-28", 5 },
    { picoquic_alpn_http_3, "h3-27", 5 },
    { picoquic_alpn_http_0_9, "hq-27", 5 },
    { picoquic_alpn_siduck, "siduck", 6 },
    { picoquic_alpn_siduck, "siduck-00", 9 },
    { picoquic_alpn_quicperf, QUICPERF_ALPN, QUICPERF_ALPN_LEN}
};

static size_t nb_alpn_list = sizeof(alpn_list) / sizeof(picoquic_alpn_list_t);

mutex network_lock;

void picoquic_client_set_alpn_list(void* tls_context)
{
    int ret = 0;

    for (size_t i = 0; i < nb_alpn_list; i++) {
        if (alpn_list[i].alpn_code == picoquic_alpn_http_3 ||
            alpn_list[i].alpn_code == picoquic_alpn_http_0_9) {
            ret = picoquic_add_proposed_alpn(tls_context, alpn_list[i].alpn_val);
            if (ret != 0) {
                DBG_PRINTF("Could not propose ALPN=%s, ret=0x%x", alpn_list[i].alpn_val, ret);
                break;
            }
        }
    }
}

typedef struct st_client_stream_ctx_t {
    h3zero_data_stream_state_t stream_state;
    uint64_t received_length;
    size_t scenario_index;
    uint64_t stream_id;
    uint64_t post_size;
    uint64_t post_sent;
    char* f_name;
    unsigned int is_open : 1;
    unsigned int flow_opened : 1;
} client_stream_ctx;

typedef struct st_client_http_ctx_t
{
    uint32_t nb_client_streams;
    int nb_open_streams;

    uint32_t total_requests;
    int cnx_id;
    int connection_closed;
    int connection_ready;
    picoquic_alpn_enum alpn;
} picoquic_http_client_callback_ctx; // Context for callback for each connections

int client_close_stream(picoquic_cnx_t * cnx,
    picoquic_http_client_callback_ctx* ctx, client_stream_ctx* stream_ctx)
{
    int ret = 0;
    if (stream_ctx != NULL && stream_ctx->is_open) {
        picoquic_unlink_app_stream_ctx(cnx, stream_ctx->stream_id);
        if (stream_ctx->f_name != NULL) {
            free(stream_ctx->f_name);
            stream_ctx->f_name = NULL;
        }
        stream_ctx->is_open = 0;
        ctx->nb_open_streams--; 
        ret = 1;
    }
    return ret;
}

int picoquic_http_client_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{   
    int ret = 0;
    uint64_t fin_stream_id = STREAM_ID_INITIAL;
    picoquic_http_client_callback_ctx*  ctx = (picoquic_http_client_callback_ctx*) callback_ctx;
    client_stream_ctx* stream_ctx = (client_stream_ctx *)v_stream_ctx;

    switch (fin_or_event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin: /* Data arrival on stream #x, maybe with fin mark */
        {
            /* Data arrival on stream #x, maybe with fin mark */
            // if (stream_ctx == NULL) {
            //     assert(ctx->stream_ctx_store.find(stream_id) != ctx->stream_ctx_store.end());
            //     stream_ctx = ctx->stream_ctx_store[stream_id];
            // }
            if (stream_ctx != NULL && stream_ctx->is_open) {
                if (ret == 0 && length > 0) {
                    switch (ctx->alpn) {
                    case picoquic_alpn_http_3: {
                        uint64_t error_found = 0;
                        size_t available_data = 0;
                        uint8_t * bytes_max = bytes + length;
                        // printf("H3 response length = %d\n", length);
                        while (bytes < bytes_max) {
                            bytes = h3zero_parse_data_stream(bytes, bytes_max, &stream_ctx->stream_state, &available_data, &error_found);
                            if (bytes == NULL) {
                                ret = picoquic_close(cnx, error_found);
                                if (ret != 0) {
                                    printf("Could not parse incoming data from stream %" PRIu64 ", error 0x%x", stream_id, error_found);                                        
                                }
                                break;
                            }
                            else if (available_data > 0) {
                                if (!stream_ctx->flow_opened){
                                    if (stream_ctx->stream_state.current_frame_length < 0x100000) {
                                        stream_ctx->flow_opened = 1;
                                    }
                                    else if (cnx->cnx_state == picoquic_state_ready) {
                                        stream_ctx->flow_opened = 1;
                                        ret = picoquic_open_flow_control(cnx, stream_id, stream_ctx->stream_state.current_frame_length);
                                    }
                                }
                                stream_ctx->received_length += available_data;
                                bytes += available_data;
                            }
                        }

                        break;
                    }
                    default:
                        DBG_PRINTF("%s", "ALPN not selected!");
                        ret = -1;
                        break;
                    }
                }

                if (fin_or_event == picoquic_callback_stream_fin) {
                    if (client_close_stream(cnx, ctx, stream_ctx)) {
                        fin_stream_id = stream_id;
                        printf("Conn %d: Stream %d ended after %d bytes\n", ctx->cnx_id, stream_id, stream_ctx->received_length);
                    }
                }
            }
            break;
        }
            break;
        case picoquic_callback_stream_reset: /* Server reset stream #x */
        case picoquic_callback_stop_sending: /* Server asks client to reset stream #x */
            // Not yet implemented
            break;
        case picoquic_callback_stateless_reset:
            printf("Received a stateless reset.\n");
            break;
        case picoquic_callback_close: /* Received connection close */
        {
            printf("Received a request to close the connection.\n");
            ctx->connection_closed = 1;
            break;
        }
        case picoquic_callback_application_close: /* Received application close */
        {
            fprintf(stdout, "Received a request to close the application.\n");
            ctx->connection_closed = 1;
            break;
        }
        case picoquic_callback_version_negotiation:
            fprintf(stdout, "Received a version negotiation request:");
            break;
        case picoquic_callback_stream_gap:
            /* Gap indication, when unreliable streams are supported */
            fprintf(stdout, "Received a gap indication.\n");
            break;
        case picoquic_callback_prepare_to_send:
            /* Used on client when posting data */
            // Not yet implemented
            break;
        case picoquic_callback_almost_ready:
        case picoquic_callback_ready:
        {
            printf("Connection %d is ready!\n", ctx->cnx_id);
            ctx->connection_ready = 1;
            break;
        }  
        case picoquic_callback_request_alpn_list:
            printf("Set ALPN list\n");
            picoquic_client_set_alpn_list((void*)bytes);
            break;
        case picoquic_callback_set_alpn:
            printf("Set ALPN\n");
            ctx->alpn = picoquic_parse_alpn((const char*)bytes);
            break;
        default:
            /* unexpected */
            break;

    }

    return ret;
}

// int client_create_connection(picoquic_quic_t* qclient, picoquic_quic_config_t* config, const char* sni, sockaddr_storage * server_addr, int cnx_id, picoquic_cnx_t* cnx, picoquic_http_client_callback_ctx* ctx) {
//     printf("Create a connection!!\n");
//     uint64_t current_time = picoquic_current_time();
    
//     cnx = picoquic_create_cnx(qclient, picoquic_null_connection_id, picoquic_null_connection_id,
//             (struct sockaddr*) server_addr, current_time,
//             config->proposed_version, sni, config->alpn, 1);

//     assert(cnx != NULL);
//     printf("Finish creating connection!\n");

//     picoquic_cnx_set_pmtud_policy(cnx, picoquic_pmtud_delayed);
//     picoquic_set_default_pmtud_policy(qclient, picoquic_pmtud_delayed);

//     // Set connection (client callback) context
//     ctx->cnx_id = cnx_id;
//     picoquic_set_callback(cnx, picoquic_http_client_callback, ctx);

//     /* Requires TP grease, for interop tests */
//     cnx->grease_transport_parameters = 1;
//     cnx->local_parameters.enable_time_stamp = 3;
//     cnx->local_parameters.do_grease_quic_bit = 1;

//     printf("Start cnx %d!\n", cnx_id);
// }

int client_open_stream(picoquic_cnx_t*cnx, picoquic_http_client_callback_ctx* ctx,
                        uint64_t stream_id, char const* doc_name) {
    int ret = 0;
    uint8_t buffer[1024];
    size_t request_length = 0;
    uint8_t name_buffer[514];
    uint8_t * path;
    size_t path_len;
    uint64_t post_size = 0;

    client_stream_ctx* stream_ctx = (client_stream_ctx*)
        malloc(sizeof(client_stream_ctx));
    
    if (stream_ctx == NULL) {
		fprintf(stdout, "Memory Error, cannot create stream context %d\n", (int)stream_id);
        return -1;
    }

    ctx->nb_open_streams++;
    ctx->nb_client_streams++;

    memset(stream_ctx, 0, sizeof(client_stream_ctx));
    // Add stream to map
    stream_ctx->stream_id = stream_id;
    stream_ctx->is_open = 1;
    // if (ctx->stream_ctx_store.find(stream_id) == ctx->stream_ctx_store.end()) {
    //     ctx->stream_ctx_store.insert({stream_id, stream_ctx});
    // }
    
    /* make sure that the doc name is properly formated */
    path = (uint8_t *)doc_name;
    path_len = strlen(doc_name);
    if (doc_name[0] != '/' && path_len + 2 <= sizeof(name_buffer)) {
        name_buffer[0] = '/';
        if (path_len > 0) {
            memcpy(&name_buffer[1], doc_name, path_len);
        }
        path = name_buffer;
        path_len++;
        name_buffer[path_len] = 0;
    }

    /* Format the protocol specific request */
    ret = h3zero_client_create_stream_request(
                buffer, sizeof(buffer), path, path_len, post_size, cnx->sni, &request_length);

    assert(ret == 0);
    // Send the request
    ret = picoquic_add_to_stream_with_ctx(cnx, stream_ctx->stream_id, buffer, request_length, 1, stream_ctx);

    return ret;
}


typedef struct st_network_request {
    int file_size;
    string url;
    int cnx_id;
    chrono::steady_clock::time_point send_time;
    chrono::steady_clock::time_point complete_time;
} network_request;

typedef struct st_quic_connection {
    picoquic_cnx_t* cnx;
    picoquic_http_client_callback_ctx* cnx_ctx;
    int cnx_id;
    int notified_ready;
    int established;
    int h3_initialized;
    int curr_stream_id;
    queue<network_request> *request_queue;
} quic_connection;

typedef struct st_sending_loop_ctx_t {
    vector<quic_connection*> *quic_cnxs;
} sending_loop_ctx;

int send_requests_from_queue(quic_connection * quic_cnx) {
    int sent = 0;
    queue<network_request> *request_queue = quic_cnx->request_queue;
    picoquic_cnx_t* cnx = quic_cnx->cnx;
    while(!request_queue->empty()) {
        network_request request = request_queue->front();
        printf("Trying to send H3 request, cnx_id=%d, size=%d, url=%s, stream_id=%d!\n", quic_cnx->cnx_id, request.file_size, request.url.c_str(), quic_cnx->curr_stream_id);
        if (picoquic_get_cnx_state(cnx) == picoquic_state_ready ||
            picoquic_get_cnx_state(cnx) == picoquic_state_client_ready_start ) {
            if (quic_cnx->h3_initialized == 0) {
                h3zero_protocol_init(cnx);
                quic_cnx->h3_initialized = 1;
            } 
            int stream_id = quic_cnx->curr_stream_id;
            // string doc_name = "/fszb-" + to_string(request.file_size); 
            string doc_name = "/" + request.url + request_filesize_delimitter + to_string(request.file_size); 
            string fname = "_" + to_string(request.file_size);
            client_open_stream(cnx, quic_cnx->cnx_ctx, stream_id, doc_name.c_str());
            quic_cnx->curr_stream_id += 4;
            sent += 1;
            network_lock.lock();
            request_queue->pop();
            network_lock.unlock();
            float queue_time = chrono::duration_cast<std::chrono::milliseconds>(chrono::steady_clock::now() - request.send_time).count();
            printf("Request sent, queue_time=%f ms\n", queue_time);
        } else {
            printf("Picoquic connection is not ready to send yet!, still in stat=%d\n", picoquic_get_cnx_state(cnx));
            break;
        }
    }
    return sent;
}

int picoquic_client_sending_loop_callback(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode, 
    void* callback_ctx, void * callback_arg)
{
    int ret = 0;
    sending_loop_ctx* ctx = (sending_loop_ctx*) callback_ctx;

    if (ctx == NULL) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    } 

    switch (cb_mode) {
        case picoquic_packet_loop_ready:
            printf("Sending loop is ready!\n");
            break;
        case picoquic_packet_loop_after_receive: /* Post receive callback */
        {   
            // printf("PICOQUIC_PACKET_LOOP_AFTER_RECEIVE, cnx state = %d\n", picoquic_get_cnx_state(ctx->cnxs->at(0)));
            for (int i = 0; i<ctx->quic_cnxs->size(); i++) {
                quic_connection * quic_cnx = ctx->quic_cnxs->at(i);
                picoquic_cnx_t *cnx = quic_cnx->cnx;
                if (picoquic_get_cnx_state(cnx) == picoquic_state_client_almost_ready && quic_cnx->notified_ready == 0) {
                    /* if almost ready, display results of negotiation */
                    if (picoquic_tls_is_psk_handshake(cnx)) {
                        printf("The session was properly resumed!\n");
                    }

                    if (cnx->zero_rtt_data_accepted) {
                        printf("Zero RTT data is accepted!\n");
                    }

                    if (cnx->alpn != NULL) {
                        fprintf(stdout, "Negotiated ALPN: %s\n", cnx->alpn);
                    }
                    fprintf(stdout, "Almost ready!\n");
                    quic_cnx->notified_ready = 1;
                }
            }
            break;
        }
        case picoquic_packet_loop_after_send:
        {
            for (int i = 0; i<ctx->quic_cnxs->size(); i++) {
                quic_connection * quic_cnx = ctx->quic_cnxs->at(i);
                picoquic_cnx_t *cnx = quic_cnx->cnx;
                if (quic_cnx->established == 0) {
                    if (picoquic_get_cnx_state(cnx) == picoquic_state_ready ||
                    picoquic_get_cnx_state(cnx) == picoquic_state_client_ready_start) {
                        printf("Connection established. Version = %x, I-CID: %llx, verified: %d\n",
                            picoquic_supported_versions[cnx->version_index].version,
                            (unsigned long long)picoquic_val64_connection_id(picoquic_get_logging_cnxid(cnx)),
                            cnx->is_hcid_verified);
                        quic_cnx->established = 1;
                        if (!quic_cnx->request_queue->empty()) {
                            send_requests_from_queue(quic_cnx);
                        }
                    }
                }
            }
            break;
        }
        case picoquic_packet_loop_wake_up:
        {
            // Send HTTP request
            int sent_req = 0;
            for (int i = 0; i<ctx->quic_cnxs->size(); i++) {
                quic_connection * quic_cnx = ctx->quic_cnxs->at(i);
                if (!quic_cnx->request_queue->empty()) {
                    send_requests_from_queue(quic_cnx);
                }
            }
            break;
        }
        case picoquic_packet_loop_port_update:
            break;
        default:
            ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
            break;
    }

    return ret;
}

int put_request_to_queue(vector<quic_connection*> *quic_cnxs, int file_size, string url, int cnx_id) {
    for (int i=0; i<quic_cnxs->size(); i++) {
        if (quic_cnxs->at(i)->cnx_id == cnx_id) {
            quic_cnxs->at(i)->request_queue->push({file_size, url, cnx_id, chrono::steady_clock::now()});
        }
    }
}

quic_connection * create_and_start_quic_connections(picoquic_quic_t * qclient, sockaddr_storage * server_addr, picoquic_quic_config_t * config, int cnx_id) {
    uint64_t current_time = picoquic_current_time();
    picoquic_cnx_t* cnx = picoquic_create_cnx(qclient, picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*) server_addr, current_time,
            config->proposed_version, config->sni, config->alpn, 1);

    assert(cnx != NULL);

    picoquic_cnx_set_pmtud_policy(cnx, picoquic_pmtud_delayed);
    picoquic_http_client_callback_ctx * ctx = new picoquic_http_client_callback_ctx;
    ctx->cnx_id = cnx_id;
    picoquic_set_callback(cnx, picoquic_http_client_callback, ctx);

    /* Requires TP grease, for interop tests */
    cnx->grease_transport_parameters = 1;
    cnx->local_parameters.enable_time_stamp = 3;
    cnx->local_parameters.do_grease_quic_bit = 1;

    printf("Start cnx %d!\n", cnx_id);
    int ret = picoquic_start_client_cnx(cnx);

    assert(ret == 0);

    quic_connection * quic_cnx = new quic_connection;
    queue<network_request> * request_queue = new queue<network_request>;    
    quic_cnx->cnx = cnx;
    quic_cnx->cnx_id = cnx_id;
    quic_cnx->cnx_ctx = ctx;
    quic_cnx->curr_stream_id = 0;
    quic_cnx->established = 0;
    quic_cnx->h3_initialized = 0;
    quic_cnx->notified_ready = 0;
    quic_cnx->request_queue = request_queue;

    return quic_cnx;
}

void close_quic_connection(quic_connection * quic_cnx) {
    delete quic_cnx->request_queue;
    picoquic_close(quic_cnx->cnx, 0);
    delete quic_cnx;
}

int main(int argc, char *argv[]) {
    std::cout << "Client started" << std::endl;

    // if(argc != 1) {
    //     std::cout << "Usage: ./http_client_toy" << std::endl;
    //     return -1;
    // }

    int ret = 0;
    char *default_server_name = "100.64.0.1";
    int default_server_port = 9000;
    char* default_server_port_str = "9000";
    
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

    // populate argc and argv
    int temp_argc = 6;
    char *temp_argv[] = {
        "./program_name",
        "-n",
        "test",
        "-D",
        "100.64.0.1",
        "9000"
    };

    picoquic_config_init(&config);
    memcpy(option_string, "A:u:f:1", 7);
    ret = picoquic_config_option_letters(option_string + 7, sizeof(option_string) - 7, NULL);

    if (ret == 0) {
        /* Get the parameters */
        while ((opt = getopt(temp_argc, temp_argv, option_string)) != -1) {
            switch (opt) {
            case 'u':
                if ((nb_packets_before_update = atoi(optarg)) <= 0) {
                    fprintf(stderr, "Invalid number of packets: %s\n", optarg);
                    printf("Error 1 in argc / argv\n");
                }
                break;
            case 'f':
                force_migration = atoi(optarg);
                if (force_migration <= 0 || force_migration > 3) {
                    fprintf(stderr, "Invalid migration mode: %s\n", optarg);
                    printf("NO2\n");
                    printf("Error 2 in argc / argv\n");
                }
                break;
            case '1':
                just_once = 1;
                break;
            case 'A':
                config.multipath_alt_config = (char*) malloc(sizeof(char) * (strlen(optarg) + 1));
                memcpy(config.multipath_alt_config, optarg, sizeof(char) * (strlen(optarg) + 1));
                printf("config.multipath_alt_config: %s\n", config.multipath_alt_config);
                break;
            default:
                if (picoquic_config_command_line(opt, &optind, temp_argc, (char const **)temp_argv, optarg, &config) != 0) {
                    printf("Error 3 in argc / argv\n");
                }
                break;
            }
        }
    }

    /* Simplified style params */
    if (optind < temp_argc) {
        server_name = temp_argv[optind++];
        is_client = 1;
    }

    if (optind < temp_argc) {
        if ((server_port = atoi(temp_argv[optind++])) <= 0) {
            fprintf(stderr, "Invalid port: %s\n", optarg);
            printf("Error 4 in argc / argv\n");
        }
    }

    if (optind < temp_argc) {
        client_scenario = temp_argv[optind++];
    }

    if (optind < temp_argc) {
        printf("Error 5 in argc / argv\n");
    }
    
    /* Run as client */
    printf("Starting Picoquic (v%s) connection to server = %s, port = %d\n", PICOQUIC_VERSION, server_name, server_port);

    picoquic_quic_t* qclient = NULL;
    vector<quic_connection*> quic_cnxs;
    uint64_t current_time = 0;
    const char* sni = config.sni;

    current_time = picoquic_current_time();

    struct sockaddr_storage server_addr;
    int is_name = 0;
    ret = picoquic_get_server_address(server_name, server_port, &server_addr, &is_name);    
    if (sni == NULL && is_name != 0) {
        sni = server_name;
    }

    if (config.ticket_file_name == NULL) {
        ret = picoquic_config_set_option(&config, picoquic_option_Ticket_File_Name, ticket_store_filename);
    }
    if (config.token_file_name == NULL) {
        ret = picoquic_config_set_option(&config, picoquic_option_Token_File_Name, token_store_filename);
    }

    // Create QUIC context
    qclient = picoquic_create_and_configure(&config, NULL, NULL, current_time, NULL);
    picoquic_set_key_log_file_from_env(qclient);
    if (config.qlog_dir != NULL) {
        picoquic_set_qlog(qclient, config.qlog_dir);
    }

    if (config.performance_log != NULL) {
        ret = picoquic_perflog_setup(qclient, config.performance_log);
    }

    printf("Finish configuring qclient!\n");
    picoquic_set_default_pmtud_policy(qclient, picoquic_pmtud_delayed);
    // picoquic_set_default_congestion_algorithm(qclient, picoquic_cubic_algorithm);
    // picoquic_set_default_pmtud_policy(qclient, picoquic_pmtud_delayed);

    
    picoquic_get_server_address(server_name, server_port, &server_addr, &is_name);
    if (sni == NULL && is_name != 0) {
        sni = server_name;
        config.sni = sni;
    }

    printf("Create connection!!\n");
    chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    quic_connection * quic_cnx = create_and_start_quic_connections(qclient, &server_addr, &config, quic_cnxs.size());
    chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    float duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    printf("Conn duration time = %f\n", duration);
    quic_cnxs.push_back(quic_cnx);

    sending_loop_ctx loop_ctx = { 0 };
    loop_ctx.quic_cnxs = &quic_cnxs;

    picoquic_packet_loop_param_t param = { 0 };
    /* In case needed, program an extra path, so we can simulate multipath or migration */
    param.local_af = server_addr.ss_family;
    param.socket_buffer_size = config.socket_buffer_size;
    param.do_not_use_gso = config.do_not_use_gso;
    param.extra_socket_required = 0;
    param.local_port = (uint16_t)picoquic_uniform_random(30000) + 20000;

    int *ret_net_thread = new int;
    picoquic_network_thread_ctx_t *net_thread_ctx = picoquic_start_network_thread(qclient, &param, picoquic_client_sending_loop_callback, &loop_ctx, ret_net_thread);

    if (net_thread_ctx == NULL) {
        printf("Could not start the network thread\n");
        return -1;
    }

    sleep(1);
    string url = "heell";
    put_request_to_queue(&quic_cnxs, 10000, url, 0);

    picoquic_wake_up_network_thread(net_thread_ctx);

    // picoquic_close(cnx, 0);

    printf("Create another connection!!\n");
    begin = std::chrono::steady_clock::now();
    quic_connection * quic_cnx2 = create_and_start_quic_connections(qclient, &server_addr, &config, quic_cnxs.size());
    end = std::chrono::steady_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    printf("Conn duration time = %f\n", duration);
    quic_cnxs.push_back(quic_cnx2);

    network_lock.lock();
    put_request_to_queue(&quic_cnxs, 200000, url, 1);
    network_lock.unlock();

    picoquic_wake_up_network_thread(net_thread_ctx); 

    sleep(2);
    network_lock.lock();
    url = "helloooo";
    put_request_to_queue(&quic_cnxs, 300000, url, 0);
    put_request_to_queue(&quic_cnxs, 1000, url, 1);
    network_lock.unlock();

    picoquic_wake_up_network_thread(net_thread_ctx); 

    sleep(5);

    // // picoquic_wake_up_network_thread(net_thread_ctx);
    // int counter = 0;
    // int max_counter = 10;
    // do {
    //     picoquic_wake_up_network_thread(net_thread_ctx);
    //     usleep(1000000);
    //     if (quic_cnx2.established == 0) {
    //         counter += 1;
    //         printf("Not yet\n");
    //     }
    // } while (quic_cnx2.established == 0 && counter < max_counter);

    // printf("Exit loop with cnx2 state = %d\n", picoquic_get_cnx_state(cnx2));

    picoquic_wake_up_network_thread(net_thread_ctx);   

    sleep(2);
    printf("Finish program!\n");

    for (int i=0; i<quic_cnxs.size(); i++) {
        close_quic_connection(quic_cnxs[i]); 
    }
    picoquic_config_clear(&config);

    return ret;
}
