/*
 *  Copyright 2021 Google, Inc
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#pragma once

#include <sys/types.h>

#include <optional>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "threadsafe_queue.h"

struct CgroupKillFD { int fd; };
struct CgroupProcsFD { int fd; };

using CgroupFD = std::optional<std::variant<CgroupKillFD, CgroupProcsFD>>;

// Close the file descriptor in cgroupfd if one exists
void close(const CgroupFD& cgroupfd);

class Reaper {
public:
    struct target_proc {
        int pidfd;
        CgroupFD cgroupfd;
        pid_t pid;
        uid_t uid;
    };
private:
    static constexpr size_t THREAD_POOL_SIZE = 2;
    ThreadsafeQueue<target_proc> reap_queue_;
    // write side of the pipe to communicate kill failures with the main thread
    int comm_fd_;
    std::vector<std::thread> thread_pool_;
    bool debug_enabled_ = false;

    ThreadsafeQueue<std::pair<uid_t, pid_t>> setprio_queue_;
    std::thread setprio_thread_;

    bool async_kill(const struct target_proc& target);
    void reaper_main();
    void victim_priority_setter();
    void notify_kill_failure(pid_t pid);
    bool debug_enabled() const { return debug_enabled_; }
public:
    Reaper() : reap_queue_(THREAD_POOL_SIZE) {}
    static bool is_reaping_supported();

    bool init(int comm_fd);
    int thread_cnt() const { return thread_pool_.size(); }
    void enable_debug(bool enable) { debug_enabled_ = enable; }

    // return true on success. errno may be set on failure.
    bool kill(const struct target_proc& target, bool synchronous);
};
