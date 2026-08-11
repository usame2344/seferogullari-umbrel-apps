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
// File:        test_vardiff.cpp
// Description: Unit tests for the variable-difficulty controller.
// Created:     2026-06-02
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include <catch2/catch_test_macros.hpp>
#include "vardiff.hpp"

#include <chrono>

using mkpool::VarDiff;
using mkpool::VarDiffConfig;
using clock_t_ = std::chrono::steady_clock;

TEST_CASE("vardiff retargets up when shares come too fast", "[vardiff]") {
    VarDiffConfig cfg;
    cfg.target_share_rate_hz = 0.2; // 1 share every 5s
    cfg.min_difficulty = 1.0;
    cfg.max_difficulty = 1e9;
    cfg.tau_seconds    = 30.0;

    VarDiff vd(cfg, 100.0);
    auto now = clock_t_::now();
    std::optional<double> last;
    // Shares at ~0.5/s (diff 100) - well above the ~0.3/s target, so difficulty
    // must rise. Send enough to cross the 72-share retarget threshold.
    for (int i = 0; i < 120; ++i) {
        now += std::chrono::seconds(2);
        if (auto nd = vd.on_share(now, vd.current())) last = nd;
    }
    REQUIRE(last.has_value());
    CHECK(*last > 100.0);
    CHECK(vd.current() > 100.0);
}

TEST_CASE("vardiff retargets down when shares stop", "[vardiff]") {
    VarDiffConfig cfg;
    cfg.target_share_rate_hz = 0.2;
    cfg.min_difficulty = 1.0;
    cfg.max_difficulty = 1e9;
    cfg.tau_seconds    = 30.0;

    VarDiff vd(cfg, 100.0);
    auto now = clock_t_::now();
    
    // Send 30 shares at start diff spaced by 10 seconds to prime and stabilize EMA
    for (int i = 0; i < 30; ++i) {
        now += std::chrono::seconds(10);
        // Priming only: the retarget suggestion is asserted after the loop.
        (void)vd.on_share(now, vd.current());
    }

    // Now stop sending shares, simulate a massive idle time of 600 seconds
    // Then send some slow shares spaced by 30 seconds to trigger retarget down
    for (int i = 0; i < 20; ++i) {
        now += std::chrono::seconds(30);
        if (auto nd = vd.on_share(now, vd.current())) {
            CHECK(*nd < 100.0);
            return;
        }
    }
    FAIL("Should have retargeted down");
}

TEST_CASE("vardiff settles a small miner near ckpool target without overshoot", "[vardiff]") {
    // Reproduces the small-Avalon reconnect loop. A miner with a FIXED hashrate
    // (so its share interval grows as difficulty rises) must converge to the
    // ckpool target (~0.3 shares/s => optimal ~= true_rate * 3.33) and then hold
    // steady - it must NOT keep climbing into the tens of thousands and stall.
    VarDiffConfig cfg;
    cfg.min_difficulty = 1024.0;
    cfg.max_difficulty = 1'000'000.0;

    VarDiff vd(cfg, 1024.0);
    auto now = clock_t_::now();

    // True hashrate = 1024 diff-units/sec (i.e. ~1 share/sec at diff 1024).
    const double hashrate = 1024.0;
    double last = 1024.0;
    for (int i = 0; i < 1200; ++i) {
        double interval = vd.current() / hashrate; // seconds to next share at current diff
        now += std::chrono::microseconds(static_cast<long long>(interval * 1e6));
        if (auto nd = vd.on_share(now, vd.current())) last = *nd;
    }

    // Should adapt up from 1024 but settle near 1024/0.3 ~= 3400, well under the
    // ~16k the old controller overshot to.
    CHECK(last > 1024.0);
    CHECK(last < 8192.0);
}

TEST_CASE("vardiff respects min/max bounds", "[vardiff]") {
    VarDiffConfig cfg;
    cfg.target_share_rate_hz = 0.2;
    cfg.min_difficulty = 10.0;
    cfg.max_difficulty = 1000.0;
    cfg.tau_seconds    = 30.0;

    VarDiff vd(cfg, 500.0);
    auto now = clock_t_::now();
    for (int i = 0; i < 200; ++i) {
        now += std::chrono::seconds(10);
        // Driving the clamp; the bounds are asserted after the loop.
        (void)vd.on_share(now, vd.current());
    }
    CHECK(vd.current() <= cfg.max_difficulty);
    CHECK(vd.current() >= cfg.min_difficulty);
}
