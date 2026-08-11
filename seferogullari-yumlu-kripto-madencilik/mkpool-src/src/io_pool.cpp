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
// File:        io_pool.cpp
// Description: Round-robin pool of Asio io_context worker threads.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "io_pool.hpp"

#include <spdlog/spdlog.h>

namespace mkpool {

IoPool::IoPool(std::size_t thread_count) {
    if (thread_count == 0) thread_count = std::max(2u, std::thread::hardware_concurrency());
    contexts_.reserve(thread_count);
    guards_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        contexts_.push_back(std::make_unique<boost::asio::io_context>(1));
        guards_.emplace_back(boost::asio::make_work_guard(*contexts_.back()));
    }
}

IoPool::~IoPool() {
    stop();
    join();
}

void IoPool::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    threads_.reserve(contexts_.size());
    for (std::size_t i = 0; i < contexts_.size(); ++i) {
        threads_.emplace_back([ctx = contexts_[i].get(), idx = i]() {
            try {
                spdlog::info("[IoPool] worker {} starting", idx);
                ctx->run();
                spdlog::info("[IoPool] worker {} exiting", idx);
            } catch (const std::exception& e) {
                spdlog::critical("[IoPool] worker {} threw: {}", idx, e.what());
            }
        });
    }
}

void IoPool::stop() {
    if (!running_.exchange(false)) return;
    for (auto& g : guards_) g.reset();
    for (auto& c : contexts_) c->stop();
}

void IoPool::join() {
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

boost::asio::io_context& IoPool::next() {
    std::size_t i = next_idx_.fetch_add(1, std::memory_order_relaxed) % contexts_.size();
    return *contexts_[i];
}

} // namespace mkpool
