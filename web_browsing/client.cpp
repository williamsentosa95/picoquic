#include <iostream>
// #include <picoquic.h>
// #include "picoquic_utils.h"
// #include "picoquic_packet_loop.h"
#include <netinet/in.h>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include "json.hpp"

#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <random>
// #include <picoquic_config.h>
// #include <democlient.h>
// #include <siduck.h>
// #include <quicperf.h>


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
// #include "http3-client.c"

using namespace std;
using namespace nlohmann;

typedef struct st_client_app_ctx_t
{
  int total_requests;
  int requests_sent;
  long bytes_requested;
  std::string request_msg;
  long total_bytes_received;
  long current_request_bytes_received;
  // std::vector<std::string> responses;
  // int *time_taken;
  long *start_times;
  long *end_times;
  std::chrono::time_point<std::chrono::system_clock> start_timestamp;
  std::chrono::time_point<std::chrono::system_clock> end_timestamp;
  std::string output_file;
} client_app_ctx_t;

int sample_client_callback(picoquic_cnx_t *cnx,
                           uint64_t stream_id, uint8_t *bytes, size_t length,
                           picoquic_call_back_event_t fin_or_event, void *callback_ctx, void *v_stream_ctx);


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

map<string, json> activities; 
map<string, vector<Activity>> affected_activities;
map<string, vector<Activity>> dependency_list; 
map<string, float> completed;
bool finished = false;
int total_object = 7;
int current_downloaded_object = 0;



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



void download_object(const string & url, const int & obj_size) {
  /* Run as client */
  picoquic_quic_config_t config;
  picoquic_config_init(&config);
  char * server_name = "localhost";
  int server_port = 9000;
  // int ret = quic_client(server_name, server_port, &config,
  //     0, 0, "index.html");
}

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
      if (job.id.find("Networking") != string::npos) {
        // Process network
        string url = activities[job.id]["url"];
        int size_bytes = activities[job.id]["transferSize"];
        float start_time = activities[job.id]["startTime"];
        float end_time = activities[job.id]["endTime"];
        float duration = end_time - start_time;
        // Download files
        printf("%d: %s, download url=%s, size=%d bytes, start=%f, end=%f, duration =%.f\n", thread_id, job.id.c_str(), url.c_str(), size_bytes, start_time, end_time, duration);
        download_object(url, size_bytes);
        printf("%d: %s completed\n", thread_id, job.id.c_str());
        // Processing is finished
        queueMutex.lock();
        current_downloaded_object += 1;
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
        // printf("Completed=%d, activities=%d\n", completed.size(), activities.size());
        // if (completed.size() >= activities.size()) {
        //   finished = true;
        //   jobCondition.notify_all();
        // }
        if (current_downloaded_object >= total_object) {
          finished = true;
          jobCondition.notify_all();
        }
        queueMutex.unlock();
      } else if (job.id.find("Loading") != string::npos || job.id.find("Scripting") != string::npos) {
        float start_time = activities[job.id]["startTime"];
        float end_time = activities[job.id]["endTime"];
        float duration = end_time - start_time;
        float current_sleep_time = 0;
        printf("%d: %s started, total sleep_duration=%f\n", thread_id, job.id.c_str(), duration);
        for (int i=0; i<affected_activities[job.id].size(); i++) {
          Activity activity = affected_activities[job.id][i];
          if (activity.duration > 0) {
            float sleep_duration = activity.duration - current_sleep_time;
            std::this_thread::sleep_for(chrono::milliseconds((int) sleep_duration));
            printf("%d: %s sleep for %f\n", thread_id, job.id.c_str(), sleep_duration);
            current_sleep_time = current_sleep_time + sleep_duration;
            assert(current_sleep_time >= 0);
            // Update the state
            queueMutex.lock();
            completed[job.id] = current_sleep_time;
            // Push a new job
            if (check_requirements(activity.activity_id, completed, dependency_list)) {
              Job job = {activity.activity_id};              
              jobQueue.emplace(job);
              jobCondition.notify_one();
            }
            queueMutex.unlock();
          } else {
            float sleep_duration = duration - current_sleep_time;
            if (sleep_duration > 0) {
              std::this_thread::sleep_for(chrono::milliseconds((int) sleep_duration));
              current_sleep_time = current_sleep_time + sleep_duration;
              // Processing is finished
              printf("%d : %s slept for %f and completed\n", thread_id, job.id.c_str(), sleep_duration);
            }
            queueMutex.lock();
            completed[job.id] = -1;
            // Push a new job
            if (check_requirements(activity.activity_id, completed, dependency_list)) {
              Job job = {activity.activity_id};
              jobQueue.emplace(job);
              jobCondition.notify_one();
            }
            // printf("Completed=%d, activities=%d\n", completed.size(), activities.size());
            // if (completed.size() >= activities.size()) {
            //   finished = true;
            //   jobCondition.notify_all();
            // }
            queueMutex.unlock();
          }
        }
      }
    }
  }
}


int main(int argc, char *argv[])
{
  std::cout << "Client started" << std::endl;
  string dep_fpath;

  if(argc > 1) {
    dep_fpath = string(argv[1]);
  } else {
    // std::cout << "Usage: ./client_browsing <webpage-dependency-graph>" << std::endl;
    // return -1;
    dep_fpath = "web_browsing/0_www.wikipedia.org.json";
    // dep_fpath = "web_browsing/0_www.cnn.com.json";
  }
  // printf("Dep file: %s\n", dep_fpath.c_str());

  json loading_logs;
  ifstream file(dep_fpath);
	if (file.is_open()) {
    loading_logs = json::parse(file);
		file.close();
  } else {
    printf("%s cannot be opened\n", dep_fpath.c_str());
    exit(0);
  }	

  printf("Log size = %u\n", loading_logs.size());

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

  printf("Activity size = %d\n", activities.size());

  // // The list of activities that the "key" activity_id affects 
  // map<string, vector<Activity>> affected_activities;
  // // The list of activities that have to be completed / started for certain period 
  // // before the "key" activity_id can be executed. 
  // map<string, vector<Activity>> dependency_list;

  /** Dependency entries looks like this:
   * "time" : time required for a2 to be executed after a1 has started, -1 means complete dependency and a1 has to finish before a2 can start
   * "a1" : activity_id
   * "a2" : activity_id
   */
  for (json node : dependency) {
    string a1 = node["a1"];
    string a2 = node["a2"];
    float duration = node["time"];
    // Remove partial dependency if a1 is "networking"
    if (a1.find("Networking") != string::npos && duration >= 0) {
      cout << "YESSS" << endl;
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

  // Print the affected activities
  printf("*** Affected activities list ***\n");
  for (auto const& node : affected_activities) {
    printf("%s -> {", node.first.c_str());
    for (int i=0; i<node.second.size(); i++) {
      cout << node.second[i].duration << "-" << node.second[i].activity_id << ",";
    }
    printf("}\n");
  }

  // Print the dependency list
  printf("*** Dependency list ***\n");
  for (auto const& node : dependency_list) {
    printf("%s -> {", node.first.c_str());
    for (int i=0; i<node.second.size(); i++) {
      cout << node.second[i].duration << "-" << node.second[i].activity_id << ",";
    }
    printf("}\n");
  }

  // map<string, float> completed;

  printf("*** Start browsing ***\n");
  Job job = {"Networking_0"};
  jobQueue.emplace(job);

  
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


  std::cout << "All jobs processed." << std::endl;







  // int ret = 0;
  // char *server_name = "100.64.0.1";
  // int server_port = 12000;
  // picoquic_quic_t *quic = NULL;
  // picoquic_cnx_t *cnx = NULL;
  // char *default_alpn = "my_custom_alpn";
  // // char *default_alpn = "application layer protocol";
  // uint64_t current_time = picoquic_current_time();

  // // Create a quic context
  // quic = picoquic_create(1, NULL, NULL, NULL, default_alpn, NULL, NULL,
  //                        NULL, NULL, NULL, current_time, NULL,
  //                        NULL, NULL, 0); // callback can be specified here too

  // if (quic == NULL)
  // {
  //   fprintf(stderr, "Could not create quic context\n");
  //   ret = -1;
  // }

  // // // Set some configurations
  // picoquic_set_default_congestion_algorithm(quic, picoquic_cubic_algorithm);
  // // // picoquic_set_key_log_file_from_env(quic);
  // // // picoquic_set_qlog(quic, qlog_dir);
  // // // picoquic_set_log_level(quic, 1);

  // // Set the server address
  // struct sockaddr_in server_address;
  // memset(&server_address, 0, sizeof(server_address));
  // server_address.sin_family = AF_INET;
  // server_address.sin_port = htons(server_port);            // Replace with the server port
  // server_address.sin_addr.s_addr = inet_addr(server_name); // Replace with the server IP address

  // // Create a connection
  // cnx = picoquic_create_cnx(quic, picoquic_null_connection_id, picoquic_null_connection_id,
  //                           (struct sockaddr *)&server_address, current_time, 0, NULL, default_alpn, 1);

  // if (cnx == NULL)
  // {
  //   fprintf(stderr, "Could not create connection context\n");
  // }

  // // Creating the client context
  // // char* message = argv[1];
  // // char message[] = "10000";

  // client_app_ctx_t *client_ctx = new client_app_ctx_t();
  // client_ctx->total_requests = atoi(argv[1]);
  // // std::cout << "Total requests: " << client_ctx->total_requests << std::endl;
  // client_ctx->requests_sent = 0;
  // client_ctx->bytes_requested = strtol(argv[2], NULL, 10);
  // // std::cout << "Bytes requested: " << client_ctx->bytes_requested << std::endl;
  // client_ctx->request_msg = std::string(argv[2]);
  // client_ctx->total_bytes_received = 0;
  // client_ctx->current_request_bytes_received = 0;
  // // client_ctx->time_taken = new int[client_ctx->total_requests];
  // client_ctx->start_times = new long[client_ctx->total_requests];
  // client_ctx->end_times = new long[client_ctx->total_requests];
  // client_ctx->output_file = std::string(argv[3]);

  // // printf("Starting connection to %s, port %d\n", server_name, server_port);

  // picoquic_set_callback(cnx, sample_client_callback, client_ctx);
  // ret = picoquic_start_client_cnx(cnx);

  // if (ret < 0)
  // {
  //   fprintf(stderr, "Could not start connection\n");
  //   ret = -1;
  // }
  // else
  // {
  //   /* Printing out the initial CID, which is used to identify log files */
  //   picoquic_connection_id_t icid = picoquic_get_initial_cnxid(cnx);
  //   // printf("Initial connection ID: ");
  //   // for (uint8_t i = 0; i < icid.id_len; i++)
  //   // {
  //   //   printf("%02x", icid.id[i]);
  //   // }
  //   // printf("\n");
  // }

  // /* Obtain the next available stream ID in the local category */
  // // int is_unidir = 0;
  // // uint64_t stream_id = picoquic_get_next_local_stream_id(cnx, is_unidir);

  // // // Timestamp
  // // client_ctx->start_timestamp = std::chrono::high_resolution_clock::now();

  // // //  some data
  // // ret = picoquic_add_to_stream(cnx, stream_id, (const uint8_t *)client_ctx->request_msg.c_str(), client_ctx->request_msg.length(), 0);
  // // client_ctx->requests_sent++;

  // // if (ret < 0)
  // // {
  // //   fprintf(stderr, "Could not send data\n");
  // // }

  // /* Wait for packets */
  // ret = picoquic_packet_loop(quic, 0, server_address.sin_family, 0, 0, 0, NULL, NULL);

  // /* Free the Client context */
  // // sample_client_free_context(&client_ctx);

  // return ret;
}

int sample_client_callback(picoquic_cnx_t *cnx,
                           uint64_t stream_id, uint8_t *bytes, size_t length,
                           picoquic_call_back_event_t fin_or_event, void *callback_ctx, void *v_stream_ctx)
{
  client_app_ctx_t *client_ctx = (client_app_ctx_t *)callback_ctx;

  switch (fin_or_event)
  {
  case picoquic_callback_stream_data: // Data received from peer on stream N
    // std::cout << "Client callback: stream data. length is " << length << std::endl;
    // std::cout << "Data: " << std::string((char *)bytes, length) << std::endl;
    {
      // std::chrono::time_point<std::chrono::system_clock> time_now = std::chrono::high_resolution_clock::now();
      // float now_ms = (time_now.time_since_epoch().count() - client_ctx->start_timestamp.time_since_epoch().count()) / 1e6;
      // Store the response and if it's the end, send another request
      if (client_ctx->current_request_bytes_received == 0)
      {
        // client_ctx->responses.push_back(std::string((char *)bytes, length));
        client_ctx->current_request_bytes_received += length;
        client_ctx->total_bytes_received += length;
      }
      else
      {
        // client_ctx->responses.back() += std::string((char *)bytes, length);
        client_ctx->current_request_bytes_received += length;
        client_ctx->total_bytes_received += length;
      }

      if (client_ctx->current_request_bytes_received == client_ctx->bytes_requested)
      {
        client_ctx->end_timestamp = std::chrono::high_resolution_clock::now();
        int req_id = client_ctx->requests_sent - 1;
        client_ctx->start_times[req_id] = client_ctx->start_timestamp.time_since_epoch().count();
        client_ctx->end_times[req_id] = client_ctx->end_timestamp.time_since_epoch().count();
        float duration = (client_ctx->end_times[req_id] - client_ctx->start_times[req_id]) / 1e6;
        client_ctx->current_request_bytes_received = 0;

        std::cout << "ID " << req_id << ", duration = " << duration << " ms" << std::endl;

        if (client_ctx->requests_sent < client_ctx->total_requests)
        {
          // std::cout << "Sending another request" << std::endl;
          client_ctx->start_timestamp = std::chrono::high_resolution_clock::now();
          picoquic_add_to_stream(cnx, stream_id, (const uint8_t *)client_ctx->request_msg.c_str(), client_ctx->request_msg.length(), 0);
          client_ctx->requests_sent++;
        }
        else
        {
          // std::cout << "All requests sent" << std::endl;
          // for (auto &response : client_ctx->responses)
          // {
          //   std::cout << "Response: " << response.length() << std::endl;
          // }

          // Write to file
          std::ofstream file(client_ctx->output_file);
          if (file.is_open())
          {
            file << "duration_ms" << std::endl;
          }

          float total_duration = 0;
          float duration = 0;
          for (int i = 0; i < client_ctx->total_requests; i++)
          {
            duration = (client_ctx->end_times[i] - client_ctx->start_times[i]) / 1e6;
            total_duration += duration;
            file << duration << std::endl;
            std::cout << duration << std::endl;
            // std::cout << client_ctx->time_taken[i] << " microseconds" << std::endl;
          }
          std::cout << "Average = " << total_duration / client_ctx->total_requests << " ms" << std::endl;

          file.close();

          // delete[] client_ctx->time_taken;
          delete[] client_ctx->start_times;
          delete[] client_ctx->end_times;
          delete client_ctx;
          exit(0);
        }
      }

      break;
    }
  case picoquic_callback_stream_fin: // Fin received from peer on stream N; data is optional
    // std::cout << "Client callback: stream fin. length is " << length << std::endl;
    break;
  case picoquic_callback_ready:
  {
    // std::cout << "Client callback: ready length " << length << std::endl;
    int is_unidir = 0;
    uint64_t stream_id = picoquic_get_next_local_stream_id(cnx, is_unidir);
    // std::cout << "Steam id:" << stream_id << std::endl;

    // Timestamp
    client_ctx->start_timestamp = std::chrono::high_resolution_clock::now();

    // Send some data
    picoquic_add_to_stream(cnx, stream_id, (const uint8_t *)client_ctx->request_msg.c_str(), client_ctx->request_msg.length(), 0);
    client_ctx->requests_sent++;

    break;
  }
  default:
    // std::cout << "Client callback: unknown event " << fin_or_event << std::endl;
    break;
  }
  return 0;
}

// two different callback context -> stream or application context

// Can do hostname resolution using picoquic_get_server_address() api

// char message[] = "Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!Hello, server!1234567890!";

// ../picoquic_sample client localhost 4433 ./temp <filename>
// ../picoquic_sample server 4433 ./ca-cert.pem ./server-key.pem ./server_files
