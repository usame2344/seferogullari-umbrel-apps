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
// File:        rate_limiter.cpp
// Description: Token-bucket per-IP rate limiting and abuse throttling.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "rate_limiter.hpp"

#include <algorithm>

namespace mkpool {

RateLimiter::RateLimiter(RateLimitConfig cfg) : cfg_(cfg) {}

void RateLimiter::refill_locked(Bucket& b, std::chrono::steady_clock::time_point now) const {
    using namespace std::chrono;
    auto elapsed = duration<double>(now - b.last).count();
    if (elapsed > 0) {
        b.tokens = std::min<double>(cfg_.accept_burst,
                                    b.tokens + elapsed * cfg_.accept_refill_per_sec);
        b.last = now;
    }
}

bool RateLimiter::allow_connection(std::string_view ip) {
    auto now = std::chrono::steady_clock::now();
    // Banned check first (shared lock).
    {
        std::shared_lock rl(mu_);
        auto bit = bans_.find(std::string(ip));
        if (bit != bans_.end() && now < bit->second.until) return false;
    }

    std::unique_lock wl(mu_);
    // Possibly expire ban
    if (auto bit = bans_.find(std::string(ip)); bit != bans_.end()) {
        if (now >= bit->second.until) bans_.erase(bit);
        else return false;
    }

    auto& b = buckets_[std::string(ip)];
    if (b.last.time_since_epoch().count() == 0) {
        b.tokens = cfg_.accept_burst;
        b.last = now;
    }
    refill_locked(b, now);

    if (b.tokens < 1.0) return false;
    if (b.open_connections >= cfg_.max_connections_per_ip) return false;
    b.tokens -= 1.0;
    ++b.open_connections;
    return true;
}

void RateLimiter::on_disconnect(std::string_view ip) {
    std::unique_lock wl(mu_);
    auto it = buckets_.find(std::string(ip));
    if (it == buckets_.end()) return;
    if (it->second.open_connections > 0) --it->second.open_connections;
}

void RateLimiter::ban(std::string_view ip, std::chrono::seconds duration) {
    auto now = std::chrono::steady_clock::now();
    std::unique_lock wl(mu_);
    bans_[std::string(ip)] = BanEntry{ now + duration };
}

bool RateLimiter::is_banned(std::string_view ip) {
    auto now = std::chrono::steady_clock::now();
    std::shared_lock rl(mu_);
    auto it = bans_.find(std::string(ip));
    if (it == bans_.end()) return false;
    return now < it->second.until;
}

bool RateLimiter::record_invalid_share(std::string_view ip) {
    auto now = std::chrono::steady_clock::now();
    std::unique_lock wl(mu_);
    auto& b = buckets_[std::string(ip)];
    if (b.invalid_window_start.time_since_epoch().count() == 0 ||
        now - b.invalid_window_start > cfg_.invalid_share_window) {
        b.invalid_window_start = now;
        b.invalid_shares = 0;
    }
    if (++b.invalid_shares >= cfg_.invalid_share_ban_threshold) {
        bans_[std::string(ip)] = BanEntry{ now + cfg_.ban_duration };
        b.invalid_shares = 0;
        return true;
    }
    return false;
}

void RateLimiter::clear_invalid(std::string_view ip) {
    std::unique_lock wl(mu_);
    auto it = buckets_.find(std::string(ip));
    if (it != buckets_.end()) {
        it->second.invalid_shares = 0;
    }
}

RateLimiter::Snapshot RateLimiter::snapshot() const {
    std::shared_lock rl(mu_);
    Snapshot s{};
    for (const auto& kv : buckets_) s.connections += kv.second.open_connections;
    s.banned = bans_.size();
    return s;
}

} // namespace mkpool
