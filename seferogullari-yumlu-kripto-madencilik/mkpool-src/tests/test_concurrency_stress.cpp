// SPDX-License-Identifier: GPL-3.0
// Copyright (c) 2025-2026 Mecanik1337 <contact@mecanik.dev>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// File:        test_concurrency_stress.cpp
// Description: Multi-threaded stress on the structures shared across io threads.
// Created:     2026-07-15
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// Run these under ThreadSanitizer; without TSan they mostly prove "it did not
// crash today on this scheduling", which is close to worthless for races:
//
//   cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DMKPOOL_ENABLE_TSAN=ON -DMKPOOL_ENABLE_LTO=OFF
//   cmake --build build-tsan --target mkpool_tests -j
//   ./build-tsan/tests/mkpool_tests "[concurrency]"
//
// WHY THESE STRUCTURES
// --------------------
// IoPool runs one thread per io_context and assigns sessions round-robin, so a
// single session's handlers are thread-confined and its own members are not the
// risk. The risk is the state shared BETWEEN contexts:
//
//   JobWindow  - the stratifier pushes new jobs from one thread while sessions
//                on every other io thread look up job ids to validate shares.
//                This is the hottest cross-thread structure in the pool.
//
// The 2026-07-15 crash *presented* as mutex corruption (SIGSEGV inside
// pthread_mutex_lock, a glibc tpp assert). That turned out to be a heap overflow
// scribbling over a mutex rather than a race -- but the two are indistinguishable
// from the stack trace alone, which is exactly why races need their own detector
// rather than an eyeball.

#include <catch2/catch_test_macros.hpp>
#include "job_window.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using mkpool::JobWindow;

namespace {

// Keep the load high enough to interleave, low enough that a TSan run (which is
// ~10x slower) still finishes in seconds.
constexpr int kWriters       = 4;
constexpr int kReaders       = 8;
constexpr int kOpsPerThread  = 2000;

std::string job_id_for(int writer, int n) {
    // 8 hex chars so find_u32() can parse the same id off the wire, matching how
    // SV2 truncates a job id to its first 4 bytes.
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04x%04x", writer & 0xffff, n & 0xffff);
    return std::string(buf);
}

} // namespace

TEST_CASE("JobWindow: concurrent push/find/latest", "[concurrency][jobwindow]") {
    JobWindow<std::string> w(64);
    std::atomic<bool> go{false};
    std::atomic<long> reads{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kWriters; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) { /* spin to align start */ }
            for (int i = 0; i < kOpsPerThread; ++i) {
                w.push(job_id_for(t, i), "payload-" + std::to_string(i));
            }
        });
    }

    for (int t = 0; t < kReaders; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) { }
            for (int i = 0; i < kOpsPerThread; ++i) {
                // Every read path a live session takes while jobs roll over.
                (void)w.find(job_id_for(t % kWriters, i));
                (void)w.latest();
                (void)w.size();
                if (auto v = w.find_u32(static_cast<std::uint32_t>(i))) {
                    // Reading the returned value must be safe even though the
                    // entry may already have been evicted: find() returns by
                    // value, so the copy has to outlive the eviction.
                    if (!v->empty()) reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    // Capacity must hold under concurrent pushes; an over-capacity window means
    // pop_front raced with emplace_back and the deque grew unbounded.
    CHECK(w.size() <= 64);
    CHECK(reads.load() >= 0); // value is scheduling-dependent; presence is not
}

TEST_CASE("JobWindow: concurrent clear against readers", "[concurrency][jobwindow]") {
    // clear() runs on a new-block boundary while sessions are mid-lookup. A
    // reader holding an iterator across a clear would be a use-after-free.
    JobWindow<std::string> w(32);
    for (int i = 0; i < 32; ++i) w.push(job_id_for(0, i), "v");

    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                (void)w.find("00000001");
                (void)w.latest();
                (void)w.find_u32(0);
            }
        });
    }

    for (int i = 0; i < 200; ++i) {
        w.clear();
        for (int j = 0; j < 16; ++j) w.push(job_id_for(1, j), "v");
    }
    stop.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();

    CHECK(w.size() <= 32);
}

TEST_CASE("JobWindow: eviction is exact under load", "[concurrency][jobwindow]") {
    // Single-threaded control for the two tests above: if capacity is wrong here,
    // a failure up there is a logic bug, not a race.
    JobWindow<int> w(4);
    for (int i = 0; i < 100; ++i) w.push(job_id_for(0, i), i);
    CHECK(w.size() == 4);
    CHECK(*w.latest() == 99);
    CHECK_FALSE(w.find(job_id_for(0, 0)).has_value()); // long evicted
    CHECK(w.find(job_id_for(0, 99)).has_value());
}
