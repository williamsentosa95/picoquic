#include <iostream>
#include <picoquic.h>
#include "picoquic_packet_loop.h"
#include "picoquic_internal.h"
#include <cmath>
#include <chrono>
#include <fstream>
#include <queue>
#include <mutex>
#include <map>

int timeout;
int wait_period;
int initial_port;
std::queue<int> queue_0;
std::queue<int> queue_1_2;
std::mutex queue_mutex_0;
std::mutex queue_mutex_1_2;

struct MsgInfo
{
  uint32_t msg_no;
  uint32_t msg_size;
  size_t bytes_received;
  MsgInfo()
  {
    this->msg_no = 0;
    this->msg_size = 0;
    this->bytes_received = 0;
  }
};

typedef struct st_receiver_app_ctx_t
{
  std::map<uint64_t, MsgInfo> msg_info;
} receiver_app_ctx_t;

int receiver_app_callback(picoquic_cnx_t *cnx,
                          uint64_t stream_id, uint8_t *bytes, size_t length,
                          picoquic_call_back_event_t fin_or_event, void *callback_ctx, void *stream_ctx);

void render_frames();

int main(int argc, char **argv)
{
  // if (((4 != argc) || (0 == atoi(argv[1])) || (0 == atoi(argv[2]))) || (0 == atoi(argv[3])))
  // {
  //   std::cout << "usage: appserver server_port timeout(s) wait_perod(ms)" << endl;
  //   return 0;
  // }
  std::cout << "Receiver started" << std::endl;

  // int port = atoi(argv[1]);
  // timeout = atoi(argv[2]);
  // wait_period = atoi(argv[3]);
  wait_period = 60;
  // initial_port = port;

  int ret = 0;
  int server_port = 12000;
  const char *server_cert = "./toy_app/ca-cert.pem";
  const char *server_key = "./toy_app/server-key.pem";
  char *default_alpn = "my_custom_alpn";
  uint64_t current_time = picoquic_current_time();

  // Server app context
  receiver_app_ctx_t *receiver_ctx = new receiver_app_ctx_t();
  // receiver_ctx->msg_info_high_priority_stream.msg_data = buffer0;
  // receiver_ctx->msg_info_low_priority_stream.msg_data = buffer1;

  // Create a quic context
  picoquic_quic_t *quic = picoquic_create(10, server_cert, server_key, NULL, default_alpn, receiver_app_callback, receiver_ctx,
                                          NULL, NULL, NULL, current_time, NULL,
                                          NULL, NULL, 0);

  if (quic == NULL)
  {
    fprintf(stderr, "Could not create quic context\n");
    return -1;
  }

  // Set some configurations
  picoquic_set_default_congestion_algorithm(quic, picoquic_cubic_algorithm);
  picoquic_set_default_multipath_option(quic, 1);  // Enable multipath
  picoquic_enable_path_callbacks_default(quic, 1); // Enable path callbacks
  // picoquic_set_key_log_file_from_env(quic);
  // picoquic_set_qlog(quic, qlog_dir);
  // picoquic_set_log_level(quic, 1);

  picoquic_packet_loop_param_t *param = new picoquic_packet_loop_param_t{0};
  param->local_port = (uint16_t)server_port;
  param->local_af = 0;
  param->dest_if = 0;
  param->socket_buffer_size = 0;
  param->do_not_use_gso = 0;
  int *ret_net_thread = new int;
  picoquic_network_thread_ctx_t *net_thread_ctx = picoquic_start_network_thread(quic, param, NULL, NULL, ret_net_thread);

  if (net_thread_ctx == NULL)
  {
    fprintf(stderr, "Could not start network thread\n");
    ret = -1;
  }

  // // Keep on waiting for packets
  // if (ret == 0)
  // {
  // ret = picoquic_packet_loop(quic, server_port, 0, 0, 0, 0, NULL, NULL);
  // }

  render_frames();

  picoquic_delete_network_thread(net_thread_ctx);

  /* Clean up */
  delete receiver_ctx;
  if (quic != NULL)
  {
    picoquic_free(quic);
  }

  return ret;
}

int receiver_app_callback(picoquic_cnx_t *cnx,
                          uint64_t stream_id, uint8_t *bytes, size_t length,
                          picoquic_call_back_event_t fin_or_event, void *callback_ctx, void *stream_ctx)
{

  receiver_app_ctx_t *receiver_ctx = (receiver_app_ctx_t *)callback_ctx;
  picoquic_stream_head_t *stream = picoquic_find_stream(cnx, stream_id);

  switch (fin_or_event)
  {
  case picoquic_callback_stream_data: // Data received from peer on stream N
  {
    if (receiver_ctx->msg_info.count(stream_id) == 0)
    {
      receiver_ctx->msg_info[stream_id] = MsgInfo();
    }
    MsgInfo &msg = receiver_ctx->msg_info[stream_id];
    // std::cout << "Data received. len: " << length << std::endl;

    while (length > 0)
    {
      if (msg.bytes_received == 0)
      {
        msg.msg_no = *(uint32_t *)bytes;
        msg.msg_size = *(uint32_t *)&bytes[4];
        length -= 8; // size of custom header
        bytes += 8;  // size of custom header
      }

      if (msg.bytes_received + length <= msg.msg_size)
      {
        msg.bytes_received += length;
        length = 0;
      }
      else
      {
        int remaining = msg.msg_size - msg.bytes_received;
        msg.bytes_received = msg.msg_size;
        length -= remaining;
        bytes += remaining;
      }

      if (msg.bytes_received == msg.msg_size)
      {
        std::cout << "Received message no: " << msg.msg_no << " size: " << msg.msg_size << std::endl;
        if (msg.msg_no % 3 != 0)
        {
          queue_mutex_1_2.lock();
          queue_1_2.push(msg.msg_no);
          queue_mutex_1_2.unlock();
        }
        else
        {
          queue_mutex_0.lock();
          queue_0.push(msg.msg_no);
          queue_mutex_0.unlock();
        }

        msg.bytes_received = 0;
      }
    }
  }
  break;
  case picoquic_callback_stream_fin: // Fin received from peer on stream N; data is optional
    std::cout << "Receiver callback: stream fin. length is " << length << std::endl;
    break;
  case picoquic_callback_path_available:
    std::cout << "Receiver callback: path available" << std::endl;
    std::cout << "Number of paths: " << cnx->nb_paths << std::endl;
    break;
  case picoquic_callback_path_suspended:
    std::cout << "Receiver callback: path suspended" << std::endl;
    break;
  case picoquic_callback_path_deleted:
    std::cout << "Receiver callback: path deleted" << std::endl;
    break;
  case picoquic_callback_path_quality_changed:
    std::cout << "Receiver callback: path quality changed" << std::endl;
    break;
  case picoquic_callback_close:
    std::cout << "Receiver callback: connection closed" << std::endl;
    break;
  default:
    std::cout << "Receiver callback: unknown event " << fin_or_event << std::endl;
    break;
  }

  return 0;
}

void render_frames()
{
  int rendered_frame_no = -1;
  int rendered_frame_max_layer = -1;
  int to_render_frame_no = -1;
  int to_render_frame_max_layer = -1;
  int msg_no = -1;
  int frame_no = -1;
  bool isIframe = false;
  bool layer_1_received;
  bool layer_2_received;
  bool got_layer_0;
  std::map<int, int> ahead_layer_0; // frame_no, msg_no
  std::map<int, int> ahead_layer_1; // frame_no, msg_no
  std::map<int, int> ahead_layer_2; // frame_no, msg_no

  while (true)
  {

    // Check if layer 0 already received. Otherwise, get it from queue
    if (ahead_layer_0.count(rendered_frame_no + 1) == 1)
    {
      msg_no = ahead_layer_0.at(rendered_frame_no + 1);
      std::fprintf(stdout, "render_error Got layer 0 from ahead map. rendered_frame_no: %d trying to render: %d msg_no: %d\n",
                   rendered_frame_no,
                   rendered_frame_no + 1,
                   msg_no);
    }
    else
    {
      queue_mutex_0.lock();
      if (queue_0.empty())
      {
        queue_mutex_0.unlock();
        continue;
      }

      // Received layer 0
      if (queue_0.size() > 1)
      {
        std::fprintf(stdout, "render_error Got more than 1 layer 0. no of layer 0 frames: %d rendered_frame_no: %d\n",
                     queue_0.size(),
                     rendered_frame_no);
      }
      msg_no = queue_0.front();
      queue_0.pop();
      queue_mutex_0.unlock();
    }

    to_render_frame_no = msg_no / 3;
    isIframe = to_render_frame_no % 10 == 0;

    // Check if it's not a previous frame
    if (to_render_frame_no < rendered_frame_no)
    {
      std::fprintf(stdout, "render_error Got frame no smaller than rendered frame no. rendered_frame_no: %d trying to render: %d msg_no: %d\n",
                   rendered_frame_no,
                   to_render_frame_no,
                   msg_no);
      continue;
    }

    // Check if P frame will be rendered
    // if (to_render_frame_no != rendered_frame_no + 1)
    if (!isIframe && to_render_frame_no != rendered_frame_no + 1)
    {
      std::fprintf(stdout, "render_error Did not get correct next frame layer 0. rendered_frame_no: %d frame no for layer 0: %d msg_no: %d\n",
                   rendered_frame_no,
                   to_render_frame_no,
                   msg_no);

      if (to_render_frame_no > rendered_frame_no)
      {
        ahead_layer_0[to_render_frame_no] = msg_no;
      }
      continue;
    }

    // Frame will be rendered. Wait for higher layers now
    int64_t layer0_ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    layer_1_received = false;
    layer_2_received = false;
    if (ahead_layer_1.count(to_render_frame_no) == 1)
    {
      std::fprintf(stdout, "render_error Got layer 1 from ahead map. rendered_frame_no: %d trying to render: %d msg_no: %d\n",
                   rendered_frame_no,
                   to_render_frame_no,
                   ahead_layer_1.at(to_render_frame_no));
      layer_1_received = true;
    }

    if (ahead_layer_2.count(to_render_frame_no) == 1)
    {
      std::fprintf(stdout, "render_error Got layer 2 from ahead map. rendered_frame_no: %d trying to render: %d msg_no: %d\n",
                   rendered_frame_no,
                   to_render_frame_no,
                   ahead_layer_2.at(to_render_frame_no));
      layer_2_received = true;
    }

    got_layer_0 = false;
    // while ((!layer_1_received || !layer_2_received))
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - layer0_ts < wait_period && (!layer_1_received || !layer_2_received))
    {
      // Check if layer 0 of next frame received or not
      // if (ahead_layer_0.count(to_render_frame_no + 1) == 1)
      // {
      //    got_layer_0 = true;
      //    std::fprintf(stdout, "render_error Got layer 0 from ahead map while waiting for layer 1&2. rendered_frame_no: %d trying to render: %d msg_no: %d\n",
      //                 rendered_frame_no,
      //                 rendered_frame_no + 1,
      //                 msg_no);
      // }
      // else
      // {
      //    queue_mutex_0.lock();
      //    while (!queue_0.empty())
      //    {
      //       int temp_msg_no = queue_0.front();
      //       queue_0.pop();
      //       ahead_layer_0[temp_msg_no / 3] = temp_msg_no;
      //       if (temp_msg_no / 3 == to_render_frame_no + 1)
      //       {
      //          std::fprintf(stdout, "render_error Got layer 0 from queue while waiting for layer 1&2. rendered_frame_no: %d trying to render: %d msg_no: %d\n",
      //                       rendered_frame_no,
      //                       rendered_frame_no + 1,
      //                       msg_no);
      //          got_layer_0 = true;
      //          break;
      //       }
      //    }
      //    queue_mutex_0.unlock();
      // }

      queue_mutex_0.lock();
      if (queue_0.size() >= 2)
      {
        got_layer_0 = true;
        std::fprintf(stdout, "render_error Got more than 1 layer 0 while waiting for layer 1&2. no of layer 0 frames: %d rendered_frame_no: %d\n",
                     queue_0.size(),
                     rendered_frame_no);
      }
      queue_mutex_0.unlock();

      if (got_layer_0)
      {
        break;
      }

      queue_mutex_1_2.lock();
      if (queue_1_2.empty())
      {
        queue_mutex_1_2.unlock();
        continue;
      }

      // Received layer 1 or 2
      msg_no = queue_1_2.front();
      frame_no = msg_no / 3;
      if (frame_no != to_render_frame_no)
      {
        std::fprintf(stdout, "render_error Did not get correct frame no for layer 1&2 rendered_frame_no: %d about to render frame no %d frame no for layer 1/2: %d msg_no: %d\n",
                     rendered_frame_no,
                     to_render_frame_no,
                     frame_no,
                     msg_no);
        if (frame_no > rendered_frame_no)
        {
          if (msg_no % 3 == 1)
          {
            ahead_layer_1[frame_no] = msg_no;
          }
          if (msg_no % 3 == 2)
          {
            ahead_layer_2[frame_no] = msg_no;
          }
        }
        queue_1_2.pop();
        queue_mutex_1_2.unlock();
        continue;
      }
      queue_1_2.pop();
      queue_mutex_1_2.unlock();

      if (msg_no % 3 == 1)
      {
        layer_1_received = true;
      }

      if (msg_no % 3 == 2)
      {
        layer_2_received = true;
      }
    }

    // cout << "Waited " << duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() - layer0_ts << " ms for higher layers" << endl;

    // Rendering frame
    if (layer_1_received && layer_2_received)
      to_render_frame_max_layer = 2;
    else if (layer_1_received)
      to_render_frame_max_layer = 1;
    else
      to_render_frame_max_layer = 0;

    rendered_frame_no = to_render_frame_no;
    isIframe ? rendered_frame_max_layer = to_render_frame_max_layer : rendered_frame_max_layer = std::min(rendered_frame_max_layer, to_render_frame_max_layer);
    int64_t render_ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(stdout, "render_frame frame_no: %d layers: %d ts_render: %lu\n",
                 rendered_frame_no,
                 rendered_frame_max_layer,
                 render_ts);
  }
}