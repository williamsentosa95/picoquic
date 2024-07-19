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

#define SERVER_ADDR "100.64.0.1"
#define SERVER_PORT 9000
#define IFACE2 "100.64.0.4"

#define ENABLE_NET_LOG 1
#define NET_LOG_PATH "/home/william/picoquic-project/net-log-mp.csv"

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

#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <random>
#include "json.hpp"
#include "url_parser.h"


#define STREAM_ID_INITIAL UINT64_MAX

using namespace std;
using namespace nlohmann;

FILE *fp = NULL;


/******************** emulated browser code ***************************/

typedef struct st_activity {
    float duration;
    string activity_id;
} Activity;

struct Job {
    string id;
};

queue<Job> jobQueue;
mutex queueMutex;
condition_variable jobCondition;
mutex dnsMutex;
mutex ongoingRequestMutex;

map<string, json> activities; 
map<string, vector<Activity>> affected_activities;
map<string, vector<Activity>> dependency_list; 
map<string, float> completed;
bool finished = false;
int total_object = 7;
int current_downloaded_object = 0;
map<string, int> host_connection_map;
int current_host_conn_id = 0;


// Check whether all required activities has been fulfilled
bool check_requirements(string activity_id, map<string, float> & completed, map<string, vector<Activity>> & dependency_list) {
    bool result = true;
    for (Activity activity : dependency_list[activity_id]) {
        if (completed.find(activity.activity_id) != completed.end()) {
            if (completed[activity.activity_id] < activity.duration) {
                result = false;
                break;
            }
        } else {
            result = false;
            break;
        }
    }
    return result;
}

bool is_sane(const string & job_id, const json & entry) {
    bool result = true;
    if (job_id.find("Networking") != string::npos) {
        result = (
                    entry.find("url") != entry.end() &&
                    entry.find("transferSize") != entry.end() &&
                    entry.find("startTime") != entry.end() &&
                    entry.find("endTime") != entry.end()
                );
    } else {
        result = (
                    entry.find("startTime") != entry.end() &&
                    entry.find("endTime") != entry.end()
                );
    }
    return result;
}

// string get_url_path(URLParser::HTTP_URL & http_url) {
//     string result = "";
//     for (auto path : http_url.path)
// 		result = result + path + "/"; 
//     return result;
// }

string get_url_path(URLParser::HTTP_URL & http_url) {
    string result = "thisisatesturl/"; 
    return result;
}

/*******************************************************************************************************/



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



/**************** New struct ******************/

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
    int connection_mp_probed;
    picoquic_alpn_enum alpn;
} picoquic_http_client_callback_ctx; // Context for callback for each connections

typedef struct st_network_request {
    int file_size;
    string url;
    int cnx_id;
    string activity_id;
    chrono::steady_clock::time_point put_to_queue_time;
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

/**************** End new struct ******************/


/*************** Global var ************/

mutex network_lock;
picoquic_quic_config_t config;
picoquic_quic_t* qclient;
vector<quic_connection*> quic_cnxs;
sending_loop_ctx loop_ctx = { 0 };
picoquic_packet_loop_param_t param = { 0 };
struct sockaddr_storage server_addr;
int* ret_net_thread;
picoquic_network_thread_ctx_t *net_thread_ctx;

map<string, network_request> ongoing_requests;

/*************** End of global var ************/

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
                        float network_time = -1;
                        float queue_time = -1;
                        string key = to_string(ctx->cnx_id) + ":" + to_string(stream_ctx->stream_id);
                        
                        ongoingRequestMutex.lock();
                        auto it = ongoing_requests.find(key);
                        string activity_id = "";
                        if (it != ongoing_requests.end()) {
                            network_time = chrono::duration_cast<std::chrono::milliseconds>(chrono::steady_clock::now() - ongoing_requests[key].send_time).count();
                            queue_time = chrono::duration_cast<std::chrono::milliseconds>(ongoing_requests[key].send_time - ongoing_requests[key].put_to_queue_time).count();
                            activity_id = ongoing_requests[key].activity_id;
                            ongoing_requests.erase(it);
                        }
                        ongoingRequestMutex.unlock();

                        printf("Conn %d, %s : Stream %d ended after %d bytes, network_time = %f ms, queue_time = %f ms\n", ctx->cnx_id, activity_id.c_str(), stream_id, stream_ctx->received_length, network_time, queue_time);
                        if (fp != NULL) {
                            fprintf(fp, "%d,%s,%d,%d,%f,%f\n", ctx->cnx_id, activity_id.c_str(), stream_id, stream_ctx->received_length, network_time, queue_time);
                        }

                        if (activity_id != "") {
                            queueMutex.lock();
                            current_downloaded_object += 1;
                            completed[activity_id] = -1;
                            // Push a new job
                            for (int i=0; i<affected_activities[activity_id].size(); i++) {
                                // Push a new job
                                Activity activity = affected_activities[activity_id][i];
                                if (check_requirements(activity.activity_id, completed, dependency_list)) {
                                    Job job = {activity.activity_id};
                                    jobQueue.emplace(job);
                                    jobCondition.notify_one();
                                }
                            }
                            printf("Completed=%d, activities=%d, last_activities=%s\n", completed.size(), activities.size(), activity_id.c_str());
                            if (completed.size() >= activities.size()) {
                                finished = true;
                                jobCondition.notify_all();
                            }
                            // if (current_downloaded_object >= total_object) {
                            //     finished = true;
                            //     jobCondition.notify_all();
                            // }
                            queueMutex.unlock();
                        } else {
                            printf("ERROR, activity id is not found!!!");
                            exit(0);
                        }
                        
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
        {
            if (ctx->connection_mp_probed != 1) {
                // probe a new path (SAT)
                struct sockaddr_storage addr_from;
                int addr_from_is_name = 0;
                struct sockaddr_storage addr_to;
                int addr_to_is_name = 0;
                int my_port = ntohs(((sockaddr_in *)&cnx->path[0]->local_addr)->sin_port);

                picoquic_get_server_address("100.64.0.1", SERVER_PORT, &addr_from, &addr_from_is_name); // remote addr
                picoquic_get_server_address("100.64.0.4", my_port, &addr_to, &addr_to_is_name);   // local addr

                int ret_probe = picoquic_probe_new_path_ex(cnx, (struct sockaddr *)&addr_from, (struct sockaddr *)&addr_to, 0, picoquic_current_time(), 0);

                if (ret_probe == 0) {
                    printf("Cnx %d : Probe successful\n", ctx->cnx_id);
                    ctx->connection_mp_probed = 1;
                } else {
                    printf("!!! Cnx %d : Probe failed\n", ctx->cnx_id);
                }
            }
        }
        case picoquic_callback_ready:
        {
            // printf("Connection %d is ready!\n", ctx->cnx_id);
            ctx->connection_ready = 1;
            // if (ctx->connection_mp_probed != 1) {
            //     // probe a new path (SAT)
            //     struct sockaddr_storage addr_from;
            //     int addr_from_is_name = 0;
            //     struct sockaddr_storage addr_to;
            //     int addr_to_is_name = 0;
            //     int my_port = ntohs(((sockaddr_in *)&cnx->path[0]->local_addr)->sin_port);

            //     picoquic_get_server_address(SERVER_ADDR, SERVER_PORT, &addr_from, &addr_from_is_name); // remote addr
            //     picoquic_get_server_address(IFACE2, my_port, &addr_to, &addr_to_is_name);   // local addr

            //     int ret_probe = picoquic_probe_new_path_ex(cnx, (struct sockaddr *)&addr_from, (struct sockaddr *)&addr_to, 0, picoquic_current_time(), 0);

            //     if (ret_probe == 0) {
            //         printf("Cnx %d : Probe successful\n", ctx->cnx_id);
            //         ctx->connection_mp_probed = 1;
            //     } else {
            //         printf("!!! Cnx %d : Probe failed\n", ctx->cnx_id);
            //     }
            // }
            break;
        }  
        case picoquic_callback_request_alpn_list:
            // printf("Set ALPN list\n");
            picoquic_client_set_alpn_list((void*)bytes);
            break;
        case picoquic_callback_set_alpn:
            // printf("Set ALPN\n");
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


int send_requests_from_queue(quic_connection * quic_cnx) {
    int sent = 0;
    queue<network_request> *request_queue = quic_cnx->request_queue;
    picoquic_cnx_t* cnx = quic_cnx->cnx;

    network_lock.lock();
    while(!request_queue->empty()) {
        network_request request = request_queue->front();
        // printf("Trying to send H3 request, cnx_id=%d, size=%d, url=%s, activity_id=%s, stream_id=%d!\n", quic_cnx->cnx_id, request.file_size, request.url.c_str(), request.activity_id.c_str(), quic_cnx->curr_stream_id);
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
            request.send_time = chrono::steady_clock::now();
            // Put to ongoing
            string key = to_string(quic_cnx->cnx_id) + ":" + to_string(stream_id);
            
            ongoingRequestMutex.lock();
            ongoing_requests[key] = request;
            ongoingRequestMutex.unlock();
            
            quic_cnx->curr_stream_id += 4;
            sent += 1;
            // float queue_time = chrono::duration_cast<std::chrono::milliseconds>(chrono::steady_clock::now() - request.put_to_queue_time).count();
            // printf("Request sent, queue_time=%f ms\n", queue_time);
            request_queue->pop();
        } else {
            // printf("Picoquic connection is not ready to send yet!, still in stat=%d\n", picoquic_get_cnx_state(cnx));
            break;
        }
    }
    network_lock.unlock();
    
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

                    // if (cnx->zero_rtt_data_accepted) {
                    //     printf("Zero RTT data is accepted!\n");
                    // }

                    // if (cnx->alpn != NULL) {
                    //     fprintf(stdout, "Negotiated ALPN: %s\n", cnx->alpn);
                    // }
                    // fprintf(stdout, "Almost ready!\n");
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
    printf("Finish starting cnx %d!\n", cnx_id);

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

int initialize_http3_client(char* server_name, int server_port) {
    int ret = 0;
    char option_string[512];
    int opt;
    char default_server_cert_file[512];
    char default_server_key_file[512];
    char* client_scenario = NULL;
    int nb_packets_before_update = 0;
    int force_migration = 0;
    int just_once = 0;
    int is_client = 0;

    // populate argc and argv
    int temp_argc = 6;
    char port[1000];
    sprintf(port, "%d", server_port);
    char *temp_argv[] = {
        "./program_name",
        "-n",
        "test",
        "-D",
        server_name,
        port
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
    printf("Initializing Picoquic (v%s) connection to server = %s, port = %s\n", PICOQUIC_VERSION, server_name, port);

    const char * sni = config.sni;
    uint64_t current_time = 0;
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
    current_time = picoquic_current_time();
    qclient = picoquic_create_and_configure(&config, NULL, NULL, current_time, NULL);
    picoquic_set_key_log_file_from_env(qclient);
    if (config.qlog_dir != NULL) {
        picoquic_set_qlog(qclient, config.qlog_dir);
    }

    if (config.performance_log != NULL) {
        ret = picoquic_perflog_setup(qclient, config.performance_log);
    }

    picoquic_set_default_multipath_option(qclient, 1);  // Enable multipath
    picoquic_enable_path_callbacks_default(qclient, 1); // Enable path callbacks e.g path available, path suspended, etc.

    loop_ctx.quic_cnxs = &quic_cnxs;
    param.local_af = server_addr.ss_family;
    param.socket_buffer_size = config.socket_buffer_size;
    param.do_not_use_gso = config.do_not_use_gso;
    param.extra_socket_required = 0;
    param.local_port = (uint16_t)picoquic_uniform_random(30000) + 20000;

    ret_net_thread = new int;
    net_thread_ctx = picoquic_start_network_thread(qclient, &param, picoquic_client_sending_loop_callback, &loop_ctx, ret_net_thread);

    if (net_thread_ctx == NULL) {
        printf("Could not start the network thread\n");
        return -1;
    }

    return ret;
}

int add_request_to_client(int filesize, string url, int cnx_id, string activity_id) {
    int ret = 0;
    bool found = false;
    
    network_lock.lock();

    for (int i=0; i<quic_cnxs.size(); i++) {
        if (quic_cnxs[i]->cnx_id == cnx_id) {
            found = true;
            quic_cnxs[i]->request_queue->push({filesize, url, cnx_id, activity_id, chrono::steady_clock::now()});
        }
    } 
    
    if (!found) {

        quic_connection * quic_cnx = create_and_start_quic_connections(qclient, &server_addr, &config, quic_cnxs.size());
        quic_cnx->request_queue->push({filesize, url, cnx_id, activity_id, chrono::steady_clock::now()});
        quic_cnxs.push_back(quic_cnx);
    }

    network_lock.unlock();

    picoquic_wake_up_network_thread(net_thread_ctx); 

    return ret;
}

void clean_up() {
    for (int i=0; i<quic_cnxs.size(); i++) {
        close_quic_connection(quic_cnxs[i]); 
    }
    free(ret_net_thread);
    picoquic_config_clear(&config);
}


int get_connection_id(string host) {
    int result = 0;
    dnsMutex.lock();
    if (host_connection_map.find(host) == host_connection_map.end()) {
        result = current_host_conn_id;
        current_host_conn_id += 1;
        host_connection_map[host] = result;
    } else {
        result = host_connection_map[host];
    }
    dnsMutex.unlock();
    return result;
}

/************* Browser thread *******************/

// Shared variables: activities, completed, dependency_list, affected_activities, finished
void browser(int thread_id) {
    int count = 0;
    Job job;
    bool acquired = false;
    printf("Start browser, thread_id=%d\n", thread_id);
    while (true) {
        acquired = false;
        std::unique_lock<std::mutex> lock(queueMutex);
        jobCondition.wait(lock, []{ return !jobQueue.empty() || finished; });
        // printf("Thread %d: free from waiting\n", thread_id);
        if (finished) {
            break;
        }
        if (!jobQueue.empty()) {
            job = jobQueue.front();
            jobQueue.pop();
            acquired = true;
        } else {
            acquired = false;
        }
        // printf("%d: Start processing job=%s\n", thread_id, job.id.c_str());
        lock.unlock();
        if (acquired) {
            assert(activities.find(job.id) != activities.end());
            if (is_sane(job.id, activities[job.id])) {
                if (job.id.find("Networking") != string::npos) {
                    // Process network
                    string url = activities[job.id]["url"];
                    int size_bytes = activities[job.id]["transferSize"];
                    float start_time = activities[job.id]["startTime"];
                    float end_time = activities[job.id]["endTime"];
                    float duration = end_time - start_time;
                    // Download files
                    printf("%d: %s, download url=%s, size=%d bytes, start=%f, end=%f, duration =%.f\n", thread_id, job.id.c_str(), url.c_str(), size_bytes, start_time, end_time, duration);
                    URLParser::HTTP_URL http_url = URLParser::Parse(url);
                    int conn_id = get_connection_id(http_url.host);
                    string path = get_url_path(http_url);
                    // printf("Host: %s, Path = %s\n", http_url.host.c_str(), path.c_str());
                    add_request_to_client(size_bytes, path, conn_id, job.id);
                } else if (job.id.find("Loading") != string::npos || job.id.find("Scripting") != string::npos) {
                    float start_time = activities[job.id]["startTime"];
                    float end_time = activities[job.id]["endTime"];
                    float duration = end_time - start_time;
                    printf("%d: %s started, total sleep_duration=%f\n", thread_id, job.id.c_str(), duration);
                    std::this_thread::sleep_for(chrono::milliseconds((int) duration));
                    queueMutex.lock();
                    completed[job.id] = -1;
                    // Push a new job
                    for (int i=0; i<affected_activities[job.id].size(); i++) {
                        // Push a new job
                        Activity activity = affected_activities[job.id][i];
                        if (check_requirements(activity.activity_id, completed, dependency_list)) {
                            Job job = {activity.activity_id};
                            jobQueue.emplace(job);
                            jobCondition.notify_one();
                        }
                    }
                    printf("Completed=%d, activities=%d, last_activities=%s, job_queue_size=%d\n", completed.size(), activities.size(), job.id.c_str(), jobQueue.size());
                    if (completed.size() >= activities.size()) {
                        finished = true;
                        jobCondition.notify_all();
                    }
                    queueMutex.unlock();
                }
            } else {
                printf("Job %s is missing json key, skip it!!\n", job.id.c_str());
                queueMutex.lock();
                completed[job.id] = -1;
                // Push a new job
                for (int i=0; i<affected_activities[job.id].size(); i++) {
                    // Push a new job
                    Activity activity = affected_activities[job.id][i];
                    if (check_requirements(activity.activity_id, completed, dependency_list)) {
                        Job job = {activity.activity_id};
                        jobQueue.emplace(job);
                        jobCondition.notify_one();
                    }
                }
                printf("Completed=%d, activities=%d, last_activities=%s\n", completed.size(), activities.size(), job.id.c_str());
                if (completed.size() >= activities.size()) {
                    finished = true;
                    jobCondition.notify_all();
                }
                queueMutex.unlock();
            }
        }
    }
}

/**************************************************************************************************/



int main(int argc, char *argv[]) {
    
    char* server_addr = SERVER_ADDR;
    int server_port = SERVER_PORT;

    fp = fopen( NET_LOG_PATH, "w" );
    if (fp != NULL) {
        fprintf(fp, "%s,%s,%s,%s,%s,%s\n", "cnx_id", "activity_id", "stream_id", "data_size", "transfer_time", "queue_time");
    }
    

    initialize_http3_client(server_addr, server_port);

    string dep_fpath;
    if(argc > 1) {
        dep_fpath = string(argv[1]);
    } else {
        // dep_fpath = "web_browsing/0_www.wikipedia.org.json";
        dep_fpath = "web_browsing/0_www.cnn.com.json";
    }

    json loading_logs;
    ifstream file(dep_fpath);
    
    if (file.is_open()) {
        loading_logs = json::parse(file);
        file.close();
    } else {
        printf("%s cannot be opened\n", dep_fpath.c_str());
    }	

    // printf("Log size = %u\n", loading_logs.size());

    json dependency;
    json painting;
    json rendering;
    json netlog;
    json critical_path;

    // map<string, json> activities;
    
    // Parse the logs
    for (json entry : loading_logs) {
        if (entry.contains("id")) {
            // cout << entry["id"] << endl;
            if (entry["id"] == "Deps") {
                dependency = entry["objs"];
            } else if (entry["id"] == "Rendering") {
                rendering = entry["objs"];
            } else if (entry["id"] == "Painting") {
                painting = entry["objs"];
            } else if (entry["id"] == "Netlog") {
                netlog = entry["objs"];
            } else {
                // Load netwokring/scripting/loading activities
                for (json obj: entry["objs"]) {
                    string activity_id = obj["activityId"];
                    if (activities.find(activity_id) == activities.end()) {
                        // Add new activity to the list
                        activities[activity_id] = obj;
                    } else {
                        // activity id is found, report this!
                        printf("Activity id = %s is already added!\n", activity_id);
                    }
                }
            }
        } else if (entry.contains("criticalPath")) {
            critical_path = entry["criticalPath"];
        }
    }

    // printf("Activity size = %d\n", activities.size());

    /** Dependency entries looks like this:
     * "time" : time required for a2 to be executed after a1 has started, -1 means complete dependency and a1 has to finish before a2 can start
     * "a1" : activity_id
     * "a2" : activity_id
     */
    for (json node : dependency) {
        string a1 = node["a1"];
        string a2 = node["a2"];
        float duration = node["time"];
        // Remove all partial dependency
        if (duration >= 0) {
            duration = -1;
        }
        
        if (affected_activities.find(a1) != affected_activities.end()) {
            Activity temp;
            temp.activity_id = a2;
            temp.duration = duration;
            affected_activities[a1].push_back(temp);
        } else {
            vector<Activity> temp_v;
            Activity temp;
            temp.activity_id = a2;
            temp.duration = duration;
            temp_v.push_back(temp);
            affected_activities[a1] = temp_v;
        }
        if (dependency_list.find(a2) != dependency_list.end()) {
            Activity temp;
            temp.activity_id = a1;
            temp.duration = duration;
            dependency_list[a2].push_back(temp);
        } else {
            vector<Activity> temp_v;
            Activity temp;
            temp.activity_id = a1;
            temp.duration = duration;
            temp_v.push_back(temp);
            dependency_list[a2] = temp_v;
        }
    }

    printf("*** Start browsing ***\n");
    Job job = {"Networking_0"};
    jobQueue.emplace(job);
    auto now = chrono::steady_clock::now();
        
    int numThreads = 10;
    // Create and start the thread pool
    vector<thread> threadPool;
    for (int i = 0; i < numThreads; ++i) {
        threadPool.emplace_back(browser, i);
    }


    // Wait for all threads in the pool to finish
    for (auto& thread : threadPool) {
        thread.join();
    }

    float duration_ms = chrono::duration_cast<std::chrono::milliseconds>(chrono::steady_clock::now() - now).count();
    printf("All jobs processed, duration=%f ms\n", duration_ms);


    return 0;
}
