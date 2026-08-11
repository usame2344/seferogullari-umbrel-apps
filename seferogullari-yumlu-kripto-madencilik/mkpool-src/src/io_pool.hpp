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
// File:        io_pool.hpp
// Description: Asio io_context worker-pool interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <boost/asio.hpp>
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace mkpool {

    class IoPool {
    public:
        explicit IoPool(std::size_t thread_count);
        ~IoPool();

        IoPool(const IoPool&) = delete;
        IoPool& operator=(const IoPool&) = delete;

        void start();
        void stop();
        void join();

        // Round-robin select an io_context for a new session.
        [[nodiscard]] boost::asio::io_context& next();

        // The "main" io_context (index 0). Used for accept loops, signals etc.
        [[nodiscard]] boost::asio::io_context& main() { return *contexts_.at(0); }

        [[nodiscard]] std::size_t thread_count() const noexcept {
            return contexts_.size();
        }

    private:
        std::vector<std::unique_ptr<boost::asio::io_context>> contexts_;
        std::vector<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>> guards_;
        std::vector<std::jthread> threads_;
        std::atomic<std::size_t> next_idx_{0};
        std::atomic<bool> running_{false};
    };

} // namespace mkpool
