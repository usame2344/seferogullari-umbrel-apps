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
// File:        rate_limiter.hpp
// Description: Token-bucket rate limiter interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mkpool {

    struct RateLimitConfig {
        // Per-IP TCP accept rate.
        // NOTE: This limiter is DDoS protection ONLY. Defaults must NOT interfere
        // with legitimate mining traffic, including very large farms or proxy
        // aggregators (NiceHash, MRR, MiningRigRentals) where thousands of rigs
        // share a single source IP. Values below tolerate >10k rigs/IP and a
        // SYN burst of ~10k while still blocking single-IP packet floods.
        std::uint32_t max_connections_per_ip{50000};
        std::uint32_t accept_burst{10000};
        double        accept_refill_per_sec{1000.0};

        // Per-session invalid-share ban. Off by default for accept_burst
        // sized bursts of bad shares (some farms produce transient bursts of
        // stale/duplicate shares after job switches). Set a high threshold so
        // only obvious abuse (a rig spewing thousands of garbage shares) bans.
        std::uint32_t invalid_share_ban_threshold{10000};
        std::chrono::seconds invalid_share_window{std::chrono::minutes(5)};
        std::chrono::seconds ban_duration{std::chrono::hours(1)};
    };

    class RateLimiter {
    public:
        explicit RateLimiter(RateLimitConfig cfg = {});

        // Connection accept policy. Returns true if accept should proceed.
        [[nodiscard]] bool allow_connection(std::string_view ip);
        void               on_disconnect(std::string_view ip);

        // Permanent ban (banned until ban_duration elapses).
        void ban(std::string_view ip, std::chrono::seconds duration);
        [[nodiscard]] bool is_banned(std::string_view ip);

        // Per-session bad-share accounting; returns true if the session should
        // be disconnected (and the IP banned).
        [[nodiscard]] bool record_invalid_share(std::string_view ip);
        void                clear_invalid(std::string_view ip);

        [[nodiscard]] const RateLimitConfig& config() const noexcept { return cfg_; }

        struct Snapshot {
            std::size_t connections;
            std::size_t banned;
        };
        [[nodiscard]] Snapshot snapshot() const;

    private:
        struct Bucket {
            double tokens;
            std::chrono::steady_clock::time_point last;
            std::uint32_t open_connections{0};
            std::uint32_t invalid_shares{0};
            std::chrono::steady_clock::time_point invalid_window_start{};
        };
        struct BanEntry {
            std::chrono::steady_clock::time_point until;
        };

        RateLimitConfig cfg_;
        mutable std::shared_mutex mu_;
        std::unordered_map<std::string, Bucket> buckets_;
        std::unordered_map<std::string, BanEntry> bans_;

        // Must be called under unique_lock(mu_).
        void refill_locked(Bucket& b, std::chrono::steady_clock::time_point now) const;
    };

} // namespace mkpool
