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
// File:        test_rate_limiter.cpp
// Description: Unit tests for the token-bucket rate limiter.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include <catch2/catch_test_macros.hpp>
#include "rate_limiter.hpp"

#include <thread>

using mkpool::RateLimiter;
using mkpool::RateLimitConfig;

TEST_CASE("connection cap per IP", "[ratelimit]") {
    RateLimitConfig cfg;
    cfg.max_connections_per_ip = 2;
    cfg.accept_burst = 100;
    cfg.accept_refill_per_sec = 1000;
    RateLimiter rl(cfg);
    REQUIRE(rl.allow_connection("1.2.3.4"));
    REQUIRE(rl.allow_connection("1.2.3.4"));
    REQUIRE_FALSE(rl.allow_connection("1.2.3.4"));
    rl.on_disconnect("1.2.3.4");
    REQUIRE(rl.allow_connection("1.2.3.4"));
}

TEST_CASE("invalid-share ban triggers", "[ratelimit]") {
    RateLimitConfig cfg;
    cfg.invalid_share_ban_threshold = 3;
    cfg.invalid_share_window = std::chrono::seconds(60);
    cfg.ban_duration         = std::chrono::seconds(60);
    cfg.max_connections_per_ip = 100;
    cfg.accept_burst = 100;
    cfg.accept_refill_per_sec = 1000;
    RateLimiter rl(cfg);
    CHECK_FALSE(rl.record_invalid_share("5.6.7.8"));
    CHECK_FALSE(rl.record_invalid_share("5.6.7.8"));
    CHECK(rl.record_invalid_share("5.6.7.8"));   // 3rd triggers ban
    CHECK(rl.is_banned("5.6.7.8"));
}

TEST_CASE("accept token bucket throttles burst", "[ratelimit]") {
    RateLimitConfig cfg;
    cfg.accept_burst = 3;
    cfg.accept_refill_per_sec = 0.0; // never refill
    cfg.max_connections_per_ip = 1000;
    RateLimiter rl(cfg);
    CHECK(rl.allow_connection("9.9.9.9"));
    CHECK(rl.allow_connection("9.9.9.9"));
    CHECK(rl.allow_connection("9.9.9.9"));
    CHECK_FALSE(rl.allow_connection("9.9.9.9"));
}
