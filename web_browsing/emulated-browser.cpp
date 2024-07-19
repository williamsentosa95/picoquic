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
            printf("%d: jobqueue size = %d\n", thread_id, jobQueue.size());
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
                    duration = duration / 2;
                    // Download files
                    printf("%d: %s, download url=%s, size=%d bytes, start=%f, end=%f, duration =%.f\n", thread_id, job.id.c_str(), url.c_str(), size_bytes, start_time, end_time, duration);
                    std::this_thread::sleep_for(chrono::milliseconds((int) duration));
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
                    printf("Completed=%d, activities=%d\n", completed.size(), activities.size());
                    if (completed.size() >= activities.size()) {
                        finished = true;
                        jobCondition.notify_all();
                    }
                    // if (current_downloaded_object >= total_object) {
                    //     finished = true;
                    //     jobCondition.notify_all();
                    // }
                    queueMutex.unlock();
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
                    printf("Completed=%d, activities=%d\n", completed.size(), activities.size());
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
                printf("Completed=%d, activities=%d\n", completed.size(), activities.size());
                if (completed.size() >= activities.size()) {
                    finished = true;
                    jobCondition.notify_all();
                }
                queueMutex.unlock();
            }
        }
    }
}


int check_activities(map<string, json> & activities, map<string, vector<Activity>> & affected_activities, map<string, vector<Activity>> & dependency_list) {
    map<string, float> completed;
    return 0;
}

string get_url_path(URLParser::HTTP_URL & http_url) {
    string result = "";
    for (auto path : http_url.path)
		result = result + "/" + path; 
    return result;
}

int main(int argc, char *argv[]) {
    std::cout << "Client started" << std::endl;
    string dep_fpath;

    if(argc > 1) {
        dep_fpath = string(argv[1]);
    } else {
        // std::cout << "Usage: ./client_browsing <webpage-dependency-graph>" << std::endl;
        // return -1;
        // dep_fpath = "web_browsing/0_www.wikipedia.org.json";
        dep_fpath = "web_browsing/0_www.cnn.com.json";
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
    string input_url = "https://www.wikipedia.org/portal/wikipedia.org/assets/img/Wikipedia-logo-v2.pngfszb-13571";
    URLParser::HTTP_URL http_url = URLParser::Parse(input_url);
    cout << "HOST = " << http_url.host << endl;
    for (auto path : http_url.path)
		std::cout << "path:[" << path << "]" << std::endl;
    Job job = {"Networking_0"};
    jobQueue.emplace(job);

    
    int numThreads = 100;
    auto now = chrono::steady_clock::now();

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
}