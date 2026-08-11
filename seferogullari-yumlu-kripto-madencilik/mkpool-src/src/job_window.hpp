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
// File:        job_window.hpp
// Description: Rolling window of recent mining jobs keyed by job id (stale-share lookup).
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace mkpool {

    // Maintain the N most recent items keyed by job id. O(N) lookup which is
    // fine because N is small (default 64). A flat vector is faster than an
    // unordered_map for this size and avoids allocation per insert.
    template <class T>
    class JobWindow {
    public:
        explicit JobWindow(std::size_t capacity = 64) : capacity_(capacity) {}

        void push(std::string job_id, T job) {
            std::lock_guard lk(mu_);
            if (items_.size() == capacity_) {
                items_.pop_front();
            }
            items_.emplace_back(std::move(job_id), std::move(job));
        }

        [[nodiscard]] std::optional<T> find(std::string_view job_id) const {
            std::lock_guard lk(mu_);
            for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
                if (it->first == job_id) return it->second;
            }
            return std::nullopt;
        }

        // Match by the 32-bit job id that Stratum V2 carries on the wire (the
        // first 4 bytes / 8 hex chars of the full string job id). SV2's job_id
        // field is only a u32, so it cannot echo the full id V1 uses; this
        // recovers the exact recent job from that truncated id rather than
        // falling back to the latest (wrong) job.
        [[nodiscard]] std::optional<T> find_u32(std::uint32_t id32) const {
            std::lock_guard lk(mu_);
            for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
                const std::string& jid = it->first;
                if (jid.size() < 8) continue;
                std::uint32_t cur = 0;
                auto res = std::from_chars(jid.data(), jid.data() + 8, cur, 16);
                if (res.ec == std::errc{} && cur == id32) return it->second;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<T> latest() const {
            std::lock_guard lk(mu_);
            if (items_.empty()) return std::nullopt;
            return items_.back().second;
        }

        void clear() {
            std::lock_guard lk(mu_);
            items_.clear();
        }

        [[nodiscard]] std::size_t size() const {
            std::lock_guard lk(mu_);
            return items_.size();
        }

    private:
        mutable std::mutex mu_;
        std::deque<std::pair<std::string, T>> items_;
        std::size_t capacity_;
    };

} // namespace mkpool
