#include <iostream>
#include <picoquic.h>
#include "picoquic_utils.h"
#include "picoquic_packet_loop.h"
#include "picoquic_internal.h"
#include <netinet/in.h>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <queue>
#include <mutex>
#include <vector>
#include <chrono>
#include <thread>
typedef struct st_sender_app_ctx_t
{
} sender_app_ctx_t;

typedef struct st_sender_loop_ctx_t
{
  picoquic_cnx_t *cnx;
  uint64_t stream_id_0;
  uint64_t stream_id_1;
} sender_loop_ctx_t;

struct FrameInfo
{
  int msg_id_;
  uint32_t layer_id_;
  int size_;
  float ssim_;
  uint32_t rate_;
};

struct CustomHeader
{
  uint32_t msg_no;
  uint32_t msg_size;
  CustomHeader(uint32_t msg_no, uint32_t msg_size)
  {
    this->msg_no = msg_no;
    this->msg_size = msg_size;
  }
};

#define BUFFER_SIZE 1000000

char buffer0[BUFFER_SIZE];
char buffer1[BUFFER_SIZE];
char buffer2[BUFFER_SIZE];
bool started = false;

std::queue<int> main2network_queue;
std::queue<int> network2main_queue;
std::mutex main2network_mutex;
std::mutex network2main_mutex;

int sender_app_callback(picoquic_cnx_t *cnx,
                        uint64_t stream_id, uint8_t *bytes, size_t length,
                        picoquic_call_back_event_t fin_or_event, void *callback_ctx, void *v_stream_ctx);

int sender_loop_callback(picoquic_quic_t *quic, picoquic_packet_loop_cb_enum cb_mode,
                         void *callback_ctx, void *callback_arg);

int main(int argc, char *argv[])
{
  // if ((5 != argc) || (0 == atoi(argv[2])) || (0 == atoi(argv[4])))
  // {
  //   std::cout << "usage: sender receiver_ip receiver_port trace_file timeout(s)" << std::endl;
  //   return 0;
  // }

  std::cout << "Sender started" << std::endl;

  int ret = 0;
  char *receiver_name = "192.168.188.128"; // Replace with the receiver IP address
  int receiver_port = 12000;
  // char *recv_ip = argv[1];
  // int recv_port = atoi(argv[2]);
  // int timeout = atoi(argv[4]);

  picoquic_quic_t *quic = NULL;
  picoquic_cnx_t *cnx = NULL;
  char *default_alpn = "my_custom_alpn";
  uint64_t current_time = picoquic_current_time();

  // Create a quic context
  quic = picoquic_create(10, NULL, NULL, NULL, default_alpn, NULL, NULL,
                         NULL, NULL, NULL, current_time, NULL,
                         NULL, NULL, 0); // callback can be specified here too

  if (quic == NULL)
  {
    fprintf(stderr, "Could not create quic context\n");
    ret = -1;
  }

  // // Set some configurations
  picoquic_set_default_congestion_algorithm(quic, picoquic_cubic_algorithm);
  picoquic_set_default_multipath_option(quic, 1);  // Enable multipath
  picoquic_enable_path_callbacks_default(quic, 1); // Enable path callbacks e.g path available, path suspended, etc.
  // // picoquic_set_key_log_file_from_env(quic);
  // // picoquic_set_qlog(quic, qlog_dir);
  // // picoquic_set_log_level(quic, 1);

  // Set the receiver address
  struct sockaddr_in receiver_address;
  memset(&receiver_address, 0, sizeof(receiver_address));
  receiver_address.sin_family = AF_INET;
  receiver_address.sin_port = htons(receiver_port);            // Replace with the receiver port
  receiver_address.sin_addr.s_addr = inet_addr(receiver_name); // Replace with the receiver IP address

  // Create a connection
  cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
                            (struct sockaddr *)&receiver_address, current_time, 0, NULL, default_alpn, 1);

  if (cnx == NULL)
  {
    fprintf(stderr, "Could not create connection context\n");
  }

  // Creating the client context
  sender_app_ctx_t *sender_ctx = new sender_app_ctx_t();

  // printf("Starting connection to %s, port %d\n", server_name, server_port);
  picoquic_set_callback(cnx, sender_app_callback, sender_ctx);
  ret = picoquic_start_client_cnx(cnx);

  if (ret < 0)
  {
    fprintf(stderr, "Could not start connection\n");
    ret = -1;
  }
  else
  {
    /* Printing out the initial CID, which is used to identify log files */
    picoquic_connection_id_t icid = picoquic_get_initial_cnxid(cnx);
    // printf("Initial connection ID: ");
    // for (uint8_t i = 0; i < icid.id_len; i++)
    // {
    //   printf("%02x", icid.id[i]);
    // }
    // printf("\n");
  }

  sender_loop_ctx_t *loop_ctx = new sender_loop_ctx_t();
  loop_ctx->cnx = cnx;
  loop_ctx->stream_id_0 = 0;
  loop_ctx->stream_id_1 = 0;

  picoquic_packet_loop_param_t *param = new picoquic_packet_loop_param_t{0};
  param->local_port = (uint16_t)0;
  param->local_af = receiver_address.sin_family;
  param->dest_if = 0;
  param->socket_buffer_size = 0;
  param->do_not_use_gso = 0;
  int *ret_net_thread = new int;
  picoquic_network_thread_ctx_t *net_thread_ctx = picoquic_start_network_thread(quic, param, sender_loop_callback, (void *)loop_ctx, ret_net_thread);

  if (net_thread_ctx == NULL)
  {
    fprintf(stderr, "Could not start network thread\n");
    ret = -1;
  }

  while (true)
  {
    network2main_mutex.lock();
    if (!network2main_queue.empty())
    {
      int data = network2main_queue.front();
      network2main_queue.pop();
      std::cout << "Received data from network thread: " << data << std::endl;
      network2main_mutex.unlock();
      break;
    }
    network2main_mutex.unlock();
  }

  FrameInfo msg;
  int gop_size = 0;
  int num_layers = 3;
  int max_bitrate = 0;

  std::map<int, std::vector<FrameInfo>> trace_arrays;
  // std::ifstream ifs(argv[3]);
  std::ifstream ifs("SVC/12000_custom.trace");
  ifs >> gop_size >> num_layers;
  while (ifs >> max_bitrate >>
         msg.msg_id_ >>
         msg.layer_id_ >>
         msg.size_ >>
         msg.rate_ >>
         msg.ssim_)
  {
    if (msg.size_ > BUFFER_SIZE)
    {
      fprintf(stderr, "Buffer size is not enough\n");
      return -1;
    }
    max_bitrate = (int)max_bitrate * 15.0 / 14.0;
    msg.rate_ = (int)msg.rate_ * 1500.0 / 1400.0;
    if (trace_arrays.find(max_bitrate) == trace_arrays.end())
    {
      trace_arrays[max_bitrate] = std::vector<FrameInfo>();
    }
    trace_arrays[max_bitrate].push_back(msg);
  }
  ifs.close();

  std::cout << "Trace file read" << std::endl;

  uint32_t frame_no = 0;
  float frame_gap = 33.333;
  int key_trace = 0;
  uint64_t ts_begin = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  uint64_t ts = ts_begin;
  uint32_t msg_no = 0;
  key_trace = 12857;
  char *buffer = nullptr;
  // key_trace = 16071;
  // key_trace = 1285;

  // while (frame_no < (uint32_t)30 * timeout)
  while (frame_no < 10)
  {
    main2network_mutex.lock();
    for (int i = 0; i < num_layers; i++)
    {
      msg_no = frame_no * num_layers + i;
      msg = trace_arrays[key_trace][msg_no % trace_arrays[key_trace].size()];
      if (i == 0)
        buffer = buffer0;
      else if (i == 1)
        buffer = buffer1;
      else if (i == 2)
        buffer = buffer2;

      ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

      std::string payload(msg.size_, 'a');
      CustomHeader header(msg_no, msg.size_);
      memcpy(buffer, &header, sizeof(header));
      memcpy(&buffer[sizeof(header)], payload.c_str(), payload.size());

      // Tells total size of the message header and payload -> total bytes to send
      main2network_queue.push(sizeof(CustomHeader) + msg.size_);

      fprintf(stdout, "send_msg msg_no: %u frame_no: %d layer_id: %d ssim: %.2f ts_send: %lu size: %d key_trace: %d\n",
              msg_no,
              msg_no / num_layers,
              msg_no % num_layers,
              msg.ssim_,
              ts,
              msg.size_,
              key_trace);
    }
    main2network_mutex.unlock();
    picoquic_wake_up_network_thread(net_thread_ctx);

    frame_no++;
    ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (ts - ts_begin < (uint64_t)(frame_no * frame_gap))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds((uint64_t)(frame_no * frame_gap) - (ts - ts_begin)));
    }
  }

  picoquic_delete_network_thread(net_thread_ctx);

  /* Clean up */
  delete sender_ctx;
  if (quic != NULL)
  {
    picoquic_free(quic);
  }
  return ret;
}

int sender_app_callback(picoquic_cnx_t *cnx,
                        uint64_t stream_id, uint8_t *bytes, size_t length,
                        picoquic_call_back_event_t fin_or_event, void *callback_ctx, void *v_stream_ctx)
{
  sender_app_ctx_t *sender_ctx = (sender_app_ctx_t *)callback_ctx;

  switch (fin_or_event)
  {
  case picoquic_callback_stream_data: // Data received from peer on stream N
    std::cout << "Sender callback: stream data. length is " << length << std::endl;
    break;
  case picoquic_callback_stream_fin: // Fin received from peer on stream N; data is optional
    std::cout << "Sender callback: stream fin. length is " << length << std::endl;
    break;
  case picoquic_callback_ready:
  {
    std::cout << "Sender callback: ready length " << length << std::endl;

    // probe a new path (SAT)
    struct sockaddr_storage addr_from;
    int addr_from_is_name = 0;
    struct sockaddr_storage addr_to;
    int addr_to_is_name = 0;

    picoquic_enable_path_callbacks(cnx, 1);
    picoquic_get_server_address("192.168.188.128", 12000, &addr_from, &addr_from_is_name); // remote addr
    picoquic_get_server_address("192.168.188.131", 0, &addr_to, &addr_to_is_name);         // local addr

    int ret_probe = picoquic_probe_new_path_ex(cnx, (struct sockaddr *)&addr_from, (struct sockaddr *)&addr_to, 0, picoquic_current_time(), 0);

    if (ret_probe == 0)
    {
      std::cout << "Probe successful" << std::endl;
    }
    else
    {
      std::cout << "Probe failed" << std::endl;
    }

    network2main_mutex.lock();
    network2main_queue.push(picoquic_get_next_local_stream_id(cnx, 0));
    network2main_mutex.unlock();
    break;
  }
  case picoquic_callback_path_available:
  {
    std::cout << "Sender callback: path available" << std::endl;
    std::cout << "Number of paths: " << cnx->nb_paths << std::endl;
    break;
  }
  case picoquic_callback_path_suspended:
    std::cout << "Sender callback: path suspended" << std::endl;
    break;
  case picoquic_callback_path_deleted:
    std::cout << "Sender callback: path deleted" << std::endl;
    break;
  case picoquic_callback_path_quality_changed:
    std::cout << "Sender callback: path quality changed" << std::endl;
    break;
  case picoquic_callback_close:
    std::cout << "Sender callback: connection closed" << std::endl;
    break;
  default:
    std::cout << "Sender callback: unknown event " << fin_or_event << std::endl;
    break;
  }
  return 0;
}

int sender_loop_callback(picoquic_quic_t *quic, picoquic_packet_loop_cb_enum cb_mode,
                         void *callback_ctx, void *callback_arg)
{
  sender_loop_ctx_t *loop_ctx = (sender_loop_ctx_t *)callback_ctx;
  switch (cb_mode)
  {
  case picoquic_packet_loop_ready:
  case picoquic_packet_loop_after_receive:
  case picoquic_packet_loop_after_send:
  case picoquic_packet_loop_port_update:
  case picoquic_packet_loop_time_check:
    break;
  case picoquic_packet_loop_wake_up:

    uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    fprintf(stdout, "picoquic: ts_send: %lu\n",
            ts);
    main2network_mutex.lock();
    if (!started)
    {
      loop_ctx->stream_id_0 = picoquic_get_next_local_stream_id(loop_ctx->cnx, 0);
      picoquic_set_stream_priority(loop_ctx->cnx, loop_ctx->stream_id_0, 1);
      std::cout << "Stream ID 0: " << loop_ctx->stream_id_0 << std::endl;
    }
    // Sending layer 0
    picoquic_add_to_stream(loop_ctx->cnx, loop_ctx->stream_id_0, (const uint8_t *)buffer0, main2network_queue.front(), 0);
    main2network_queue.pop();

    if (!started)
    {
      loop_ctx->stream_id_1 = picoquic_get_next_local_stream_id(loop_ctx->cnx, 0);
      picoquic_set_stream_priority(loop_ctx->cnx, loop_ctx->stream_id_1, 1);
      std::cout << "Stream ID 1: " << loop_ctx->stream_id_1 << std::endl;
      started = true;
    }

    // Sending layer 1
    picoquic_add_to_stream(loop_ctx->cnx, loop_ctx->stream_id_1, (const uint8_t *)buffer1, main2network_queue.front(), 0);
    main2network_queue.pop();
    // Sending layer 2
    picoquic_add_to_stream(loop_ctx->cnx, loop_ctx->stream_id_1, (const uint8_t *)buffer2, main2network_queue.front(), 0);
    main2network_queue.pop();
    main2network_mutex.unlock();
    break;
  }

  return 0;
}
