#include <iostream>
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
#include "url_parser.h"

using namespace std;
using namespace nlohmann;

typedef struct st_activity {
    float duration;
    string activity_id;
} Activity;

struct Job {
    string id;
    vector<string> visited;
    float duration_spent;
};

queue<Job> jobQueue;
mutex queueMutex;
condition_variable jobCondition;

map<string, json> activities; 

map<string, int> dependency_degree_mp;
map<string, vector<Activity>> dependency_graph_mp;


map<string, int> resource_dependency_degree_mp;
map<string, vector<Activity>> resource_dependency_graph_mp;

set<string> completed;
bool finished = false;

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

// Shared variables: activities, completed, dependency_list, affected_activities, finished
void browser(int thread_id, const set<string> & critical_path) {
    int count = 0;
    Job job;
    bool acquired = false;
    bool newJobAvailable = false;
    printf("Start browser, thread_id=%d\n", thread_id);
    while (true) {
        acquired = false;
        std::unique_lock<std::mutex> lock(queueMutex);
        jobCondition.wait(lock, []{ return !jobQueue.empty() || finished; });
        // printf("Thread %d: free from waiting\n", thread_id);
        if (finished) {
            lock.unlock();
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
            /*** Process the job ***/
            if (is_sane(job.id, activities[job.id])) {
                if (job.id.find("Networking") != string::npos) {
                    // Process network
                    string url = activities[job.id]["url"];
                    int size_bytes = activities[job.id]["transferSize"];
                    float start_time = activities[job.id]["startTime"];
                    float end_time = activities[job.id]["endTime"];
                    float duration = end_time - start_time;
                    // if (critical_path.find(job.id) != critical_path.end() && job.id.find("Networking_0") != string::npos) {
                    //     duration = 0;
                    // }
                    // if (critical_path.find(job.id) != critical_path.end()) {
                    //     duration = 0;
                    // }
                    // Download files
                    printf("%d: %s, download url=%s, size=%d bytes, start=%f, end=%f, duration =%.f\n", thread_id, job.id.c_str(), url.c_str(), size_bytes, start_time, end_time, duration);
                    std::this_thread::sleep_for(chrono::milliseconds((int) duration));
                } else if (job.id.find("Loading") != string::npos || job.id.find("Scripting") != string::npos) {
                    float start_time = activities[job.id]["startTime"];
                    float end_time = activities[job.id]["endTime"];
                    float duration = end_time - start_time;
                    printf("%d: %s started, total sleep_duration=%f\n", thread_id, job.id.c_str(), duration);
                    std::this_thread::sleep_for(chrono::milliseconds((int) duration));
                }
            } else {
                printf("!!! Job %s is missing json key, skip it!!\n", job.id.c_str());
            }

            /*** Enter critical section ***/
            queueMutex.lock();
            completed.emplace(job.id);
            // Reduce dependency degree and push new job (if any)
            newJobAvailable = false;
            for (auto const & node : dependency_graph_mp[job.id]) {
                assert(dependency_degree_mp[node.activity_id] > 0);
                dependency_degree_mp[node.activity_id] -= 1;
                if (dependency_degree_mp[node.activity_id] == 0) {
                    Job temp = {node.activity_id};
                    jobQueue.emplace(temp);
                    newJobAvailable = true;
                }
            }
            // Check whether all jobs are completed
            if (completed.size() >= activities.size()) {
                finished = true;
            }
            queueMutex.unlock();
            /*** Critical section ends */

            if (newJobAvailable || finished) {
                jobCondition.notify_all();
            }
        }
    }
}


string get_url_path(URLParser::HTTP_URL & http_url) {
    string result = "";
    for (auto path : http_url.path)
		result = result + "/" + path; 
    return result;
}

// void build_resource_dependency_graph(map<string>)

int main(int argc, char *argv[]) {
    std::cout << "Client started" << std::endl;
    string dep_fpath;

    if(argc > 1) {
        dep_fpath = string(argv[1]);
    } else {
        dep_fpath = "web_browsing/dep_graphs/0_www.glassdoor.com.json";
    }
    // printf("Dep file: %s\n", dep_fpath.c_str());

    json loading_logs;
    ifstream file(dep_fpath);
    
    if (file.is_open()) {
        loading_logs = json::parse(file);
        file.close();
    } else {
        printf("%s cannot be opened\n", dep_fpath.c_str());
    }	

    printf("Log size = %u\n", loading_logs.size());

    json dependency;
    json painting;
    json rendering;
    json netlog;
    // json critical_path;
    set<string> critical_path;
    
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
                        // printf("Activities id = %s\n", activity_id.c_str());
                    } else {
                        // activity id is found, report this!
                        printf("Activity id = %s is already added!\n", activity_id);
                    }
                }
            }
        } else if (entry.contains("criticalPath")) {
            for (json cp : entry["criticalPath"]) {
                critical_path.emplace(cp);
            }
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
        // Remove all partial dependency
        if (duration >= 0) {
            duration = -1;
        }
        
        // Add to dep graph
        if (dependency_graph_mp.find(a1) == dependency_graph_mp.end()) {
            vector<Activity> temp_v;
            dependency_graph_mp[a1] = temp_v;
        } 
        Activity temp;
        temp.activity_id = a2;
        temp.duration = duration;
        dependency_graph_mp[a1].push_back(temp);
        
        // Increase the dep degree
        if (dependency_degree_mp.find(a2) == dependency_degree_mp.end()) {
            dependency_degree_mp[a2] = 0;
        }
        dependency_degree_mp[a2] += 1;
    }

    // Print the dependency list
    printf("*** Dependency degree ***\n");
    for (auto const& node : dependency_degree_mp) {
        printf("%s : %d\n", node.first.c_str(), node.second);
    }

    // map<string, set<string>> dep_on_mp;
    // // Build networking (download resource) dependency graph
    // for (json node : dependency) {
    //     string a1 = node["a1"];
    //     string a2 = node["a2"];
        
    //     if (dep_on_mp.find(a2) == dep_on_mp.end()) {
    //         set<string> temp;
    //         dep_on_mp[a2] = temp;
    //     }
    //     dep_on_mp[a2].emplace(a1)

        

    //     // Remove all partial dependency
    //     if (duration >= 0) {
    //         duration = -1;
    //     }
        
    //     // Add to dep graph
    //     if (dependency_graph_mp.find(a1) == dependency_graph_mp.end()) {
    //         vector<Activity> temp_v;
    //         resource_dependency_graph_mp[a1] = temp_v;
    //     } 
    //     Activity temp;
    //     temp.activity_id = a2;
    //     temp.duration = duration;
    //     dependency_graph_mp[a1].push_back(temp);
        
    //     // Increase the dep degree
    //     if (dependency_degree_mp.find(a2) == dependency_degree_mp.end()) {
    //         dependency_degree_mp[a2] = 0;
    //     }
    //     dependency_degree_mp[a2] += 1;
    // }

    // for (auto const& activity : activities) {
    //     if (dependency_degree_mp.find(activity.first) == dependency_degree_mp.end()) {
    //         string job_id = activity.first;
    //         Job job = {activity.first, {}, 0};
    //         jobQueue.emplace(job);
    //     }
    // }

    // int step_id = 0;
    // while (!jobQueue.empty()) {
    //     Job job = jobQueue.front();
    //     float total_duration = job.duration_spent;
    //     vector<string> visited = job.visited;
    //     jobQueue.pop();
    //     // printf("%d: %s\n", step_id, job.id.c_str());
    //     step_id++;
    //     completed.emplace(job.id);
    //     visited.push_back(job.id);
        
    //     float start_time = activities[job.id]["startTime"];
    //     float end_time = activities[job.id]["endTime"];
    //     float duration = end_time - start_time;
    //     if (job.id.find("Networking") != string::npos) {
    //         duration = 60;
    //     }

    //     total_duration += duration;

    //     if (dependency_graph_mp.find(job.id) == dependency_graph_mp.end()) {
    //         string nodes = "";
    //         for (const string & node : visited) {
    //             nodes = nodes + node + ",";
    //         }
    //         printf("DUR=%.2f - %s : %s\n", total_duration, job.id.c_str(), nodes.c_str());
    //     }

    //     // Reduce dep degree
    //     for (auto const & node : dependency_graph_mp[job.id]) {
    //         assert(dependency_degree_mp[node.activity_id] > 0);
    //         dependency_degree_mp[node.activity_id] -= 1;
    //         if (dependency_degree_mp[node.activity_id] == 0) {
    //             Job temp = {node.activity_id, visited, total_duration};
    //             jobQueue.emplace(temp);
    //         }
    //     }
    // }

    // critical_path = {"Networking_0", "Networking_50", "Networking_52"};

    // printf("Networking critical path stat\n");
    // for (const string & id : critical_path) {
    //     int size_bytes = activities[id]["transferSize"];
    //     // string mimeType = activities[id]["mimeType"];
    //     // printf("%s size=%d, type=%s\n", id.c_str(), size_bytes, mimeType.c_str());
    //     printf("%s size=%d, size=%d\n", id.c_str(), size_bytes);
    // }

    // // Print activity list
    // printf("*** Activities ***\n");
    // for (auto const& activity : activities) {
    //     printf("%s\n", activity.first.c_str());
    // }

    // // Print the affected activities
    // printf("*** Dependency list ***\n");
    // for (auto const& node : dependency_graph_mp) {
    //     printf("%s -> {", node.first.c_str());
    //     for (int i=0; i<node.second.size(); i++) {
    //         cout << node.second[i].duration << "-" << node.second[i].activity_id << ",";
    //     }
    //     printf("}\n");
    // }

    // printf("*** Browse ***\n");
    // // Populate the first job (activity with zero dependency degree)
    // for (auto const& activity : activities) {
    //     if (dependency_degree_mp.find(activity.first) == dependency_degree_mp.end()) {
    //         Job job = {activity.first};
    //         jobQueue.emplace(job);
    //     }
    // }

    // printf("*** Unvisited activiy id ***\n");
    // for (auto const& activity : activities) {
    //     if (completed.find(activity.first) == completed.end()) {
    //         int dep_degree = 0;
    //         if (dependency_degree_mp.find(activity.first) != dependency_degree_mp.end()) {
    //             dep_degree = dependency_degree_mp[activity.first];
    //         }
    //         printf("%s:%d\n", activity.first.c_str(), dep_degree);
    //     }
    // }

    // printf("*** Start browsing ***\n");
    
    // // Populate the first job (activity with zero dependency degree)
    // for (auto const& activity : activities) {
    //     if (dependency_degree_mp.find(activity.first) == dependency_degree_mp.end()) {
    //         Job job = {activity.first};
    //         jobQueue.emplace(job);
    //     }
    // }
    
    // int numThreads = 10;
    // auto now = chrono::steady_clock::now();

    // // Create and start the thread pool
    // vector<thread> threadPool;
    // for (int i = 0; i < numThreads; ++i) {
    //     threadPool.emplace_back(browser, i, critical_path);
    // }

    // // Wait for all threads in the pool to finish
    // for (auto& thread : threadPool) {
    //         thread.join();
    // }


    // float duration_ms = chrono::duration_cast<std::chrono::milliseconds>(chrono::steady_clock::now() - now).count();
    // printf("All jobs processed, duration=%f ms\n", duration_ms);
}