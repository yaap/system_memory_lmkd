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

#define LOG_TAG "lowmemorykiller"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/pidfd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <mutex>

#include <log/log.h>
#include <processgroup/processgroup.h>
#include <system/thread_defs.h>

#include "reaper.h"

#define NS_PER_MS (NS_PER_SEC / MS_PER_SEC)

#ifndef __NR_process_mrelease
#define __NR_process_mrelease 448
#endif

static int process_mrelease(int pidfd, unsigned int flags) {
    return syscall(__NR_process_mrelease, pidfd, flags);
}

static inline long get_time_diff_ms(struct timespec *from,
                                    struct timespec *to) {
    return (to->tv_sec - from->tv_sec) * (long)MS_PER_SEC +
           (to->tv_nsec - from->tv_nsec) / (long)NS_PER_MS;
}

static void set_process_group_and_prio(uid_t uid, pid_t pid,
                                       const std::vector<std::string>& profiles, int prio) {
    DIR* d;
    char proc_path[PATH_MAX];
    struct dirent* de;

    if (!SetProcessProfilesCached(uid, pid, profiles)) {
        ALOGW("Failed to set task profiles for the process (%d) being killed", pid);
    }

    snprintf(proc_path, sizeof(proc_path), "/proc/%d/task", pid);
    if (!(d = opendir(proc_path))) {
        ALOGW("Failed to open %s; errno=%d: process pid(%d) might have died", proc_path, errno,
              pid);
        return;
    }

    while ((de = readdir(d))) {
        pid_t t_pid;

        if (de->d_name[0] == '.') continue;
        t_pid = atoi(de->d_name);

        if (!t_pid) {
            ALOGW("Failed to get t_pid for '%s' of pid(%d)", de->d_name, pid);
            continue;
        }

        if (setpriority(PRIO_PROCESS, t_pid, prio) && errno != ESRCH) {
            ALOGW("Unable to raise priority of killing t_pid (%d): errno=%d", t_pid, errno);
        }
    }
    closedir(d);
}

void Reaper::victim_priority_setter() {
    pid_t tid = gettid();

    // Ensure the thread does not use little cores
    if (!SetTaskProfiles(tid, {"CPUSET_SP_FOREGROUND"}, true)) {
        ALOGE("Failed to assign cpuset to the priority setter thread");
    }

    if (setpriority(PRIO_PROCESS, tid, ANDROID_PRIORITY_HIGHEST)) {
        ALOGW("Unable to raise priority of the priority setter thread (%d): errno=%d", tid, errno);
    }

    for (;;) {
        auto [uid, pid] = setprio_queue_.pop();

        set_process_group_and_prio(uid, pid,
                                   {"CPUSET_LMKD_REAP_TARGET", "SCHED_LMKD_REAP_TARGET"},
                                   ANDROID_PRIORITY_NORMAL);
    }
}

static int kill_cgroup_or_process(const Reaper::target_proc& target) {
    // Try a cgroup kill first
    if (!sendSignalToProcessGroup(target.uid, target.pid, SIGKILL)) {
        // Most, *but not all* processes are in their own cgroups managed by Android, for example
        // children of adbd. For these processes, the best thing we can do is kill the individual
        // process since we don't want to kill the entire cgroup.
        return pidfd_send_signal(target.pidfd, SIGKILL, NULL, 0);
    }

    return 0;
}

void Reaper::reaper_main() {
    struct timespec start_tm, end_tm;
    pid_t tid = gettid();

    // Ensure the thread does not use little cores
    if (!SetTaskProfiles(tid, {"CPUSET_SP_FOREGROUND"}, true)) {
        ALOGE("Failed to assign cpuset to the reaper thread");
    }

    if (setpriority(PRIO_PROCESS, tid, ANDROID_PRIORITY_HIGHEST)) {
        ALOGW("Unable to raise priority of the reaper thread (%d): errno=%d", tid, errno);
    }

    for (;;) {
        Reaper::target_proc target = reap_queue_.pop();

        if (debug_enabled()) {
            clock_gettime(CLOCK_MONOTONIC_COARSE, &start_tm);
        }

        if (kill_cgroup_or_process(target)) {
            // Inform the main thread about failure to kill
            notify_kill_failure(target.pid);
            goto done;
        }

        setprio_queue_.push({target.uid, target.pid});

        if (process_mrelease(target.pidfd, 0)) {
            ALOGE("process_mrelease %d failed: %s", target.pid, strerror(errno));
            goto done;
        }
        if (debug_enabled()) {
            clock_gettime(CLOCK_MONOTONIC_COARSE, &end_tm);
            ALOGI("Process %d was reaped in %ldms", target.pid,
                  get_time_diff_ms(&start_tm, &end_tm));
        }

done:
        close(target.pidfd);
        reap_queue_.request_complete();
    }
}

bool Reaper::is_reaping_supported() {
    static enum {
        UNKNOWN,
        SUPPORTED,
        UNSUPPORTED
    } reap_support = UNKNOWN;

    if (reap_support == UNKNOWN) {
        if (process_mrelease(-1, 0) && errno == ENOSYS) {
            reap_support = UNSUPPORTED;
        } else {
            reap_support = SUPPORTED;
        }
    }
    return reap_support == SUPPORTED;
}

bool Reaper::init(int comm_fd) {
    char name[16];
    struct sched_param param = {
        .sched_priority = 0,
    };

    if (!thread_pool_.empty()) {
        // init should not be called multiple times
        return false;
    }

    // The work in this thread is serialized in the kernel because of the cgroup mutex,
    // so only one thread even if there are multiple reapers.
    setprio_thread_ = std::thread(&Reaper::victim_priority_setter, this);
    if (pthread_setschedparam(setprio_thread_.native_handle(), SCHED_OTHER, &param)) {
        ALOGW("set SCHED_OTHER failed %s", strerror(errno));
    }

    if (pthread_setname_np(setprio_thread_.native_handle(), "lmkd_setprio")) {
        ALOGW("pthread_setname_np failed: %s", strerror(errno));
    }

    thread_pool_.reserve(THREAD_POOL_SIZE);
    for (unsigned int i = 0; i < THREAD_POOL_SIZE; i++) {
        thread_pool_.push_back(std::thread(&Reaper::reaper_main, this));

        if (pthread_setschedparam(thread_pool_.back().native_handle(), SCHED_OTHER, &param)) {
            ALOGW("set SCHED_OTHER failed %s", strerror(errno));
        }
        snprintf(name, sizeof(name), "lmkd_reaper%d", i);
        if (pthread_setname_np(thread_pool_.back().native_handle(), name)) {
            ALOGW("pthread_setname_np failed: %s", strerror(errno));
        }
    }

    comm_fd_ = comm_fd;
    return true;
}

bool Reaper::async_kill(const struct target_proc& target) {
    // Required for process_mrelease
    if (target.pidfd < 0) {
        return false;
    }

    if (thread_pool_.empty()) {
        return false;
    }

    // Duplicate pidfd instead of reusing the original one to avoid synchronization and refcounting
    // when both reaper and main threads are using or closing the pidfd
    int pidfd = dup(target.pidfd);
    bool ret = reap_queue_.push({pidfd, target.pid, target.uid});
    if (!ret) close(pidfd);

    return ret;
}

int Reaper::kill(const struct target_proc& target, bool synchronous) {
    if (!synchronous && async_kill(target)) {
        // we assume the kill will be successful and if it fails we will be notified
        return 0;
    }

    return kill_cgroup_or_process(target);
}

void Reaper::notify_kill_failure(pid_t pid) {
    static std::mutex mtx;
    std::scoped_lock lock(mtx);

    ALOGE("Failed to kill process %d", pid);
    if (TEMP_FAILURE_RETRY(write(comm_fd_, &pid, sizeof(pid))) != sizeof(pid)) {
        ALOGE("thread communication write failed: %s", strerror(errno));
    }
}
