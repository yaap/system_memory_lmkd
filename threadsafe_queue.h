/*
 *  Copyright 2025 Google, Inc
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

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

template <typename T>
class ThreadsafeQueue {
public:
    ThreadsafeQueue() = default;
    ThreadsafeQueue(size_t max_elements)
        : max_elements_(max_elements), active_elements_(0) {}

    bool push(T&& e) {
        std::scoped_lock lock(mtx_);
        if (active_elements_) {
            if (*active_elements_ >= *max_elements_) return false;
            (*active_elements_)++;
        }

        q_.push_back(std::forward<T>(e));
        cv_.notify_one();
        return true;
    }

    T pop() {
        T ret;

        std::unique_lock lock(mtx_);
        cv_.wait(lock, [this]{return !q_.empty();});
        ret = std::move(q_.front());
        q_.pop_front();

        return ret;
    }

    void request_complete() {
        if (!active_elements_) return;

        std::scoped_lock lock(mtx_);
        (*active_elements_)--;
    }
private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<T> q_;
    std::optional<size_t> max_elements_;
    std::optional<size_t> active_elements_;
};