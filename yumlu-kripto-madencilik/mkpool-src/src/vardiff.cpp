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
// File:        vardiff.cpp
// Description: Variable-difficulty controller implementation.
//
//              The retarget math (decaying dsps averages and time-bias warm-up) re-implements
//              ckpool's vardiff (Con Kolivas, GPLv3): decay_time() from src/libckpool.c and
//              time_bias()/add_submit() from src/stratifier.c. The surrounding engine is original.
// Created:     2026-06-02
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "vardiff.hpp"

#include <algorithm>
#include <cmath>

namespace mkpool {

namespace {

// ckpool tuning constants (src/stratifier.c add_submit). The target product is
// ~0.3 shares/second: optimal = dsps * (1/0.3) and the hysteresis band spans
// 0.5x..1.333x of that target. Retarget only after enough shares/time to have a
// stable estimate. These are deliberately fixed to match ckpool's behaviour.
constexpr double kOptimalMultiplier = 3.33;   // 1 / 0.3
constexpr double kDrrLow            = 0.15;   // 0.3 * 0.5
constexpr double kDrrHigh           = 0.40;   // 0.3 * 1.333
constexpr int    kSharesPerCheck    = 72;     // 0.3/s * 240s
constexpr double kCheckSeconds      = 240.0;
constexpr double kFastWindowSeconds = 60.0;   // dsps1 bias period
constexpr double kSlowWindowSeconds = 300.0;  // dsps5 bias period

// ckpool src/libckpool.c decay_time(): exponentially decaying average of a rate.
void decay_time(double& f, double fadd, double fsecs, double interval) {
    if (fsecs <= 0.0) return;
    double dexp = fsecs / interval;
    if (dexp > 36.0) dexp = 36.0; // bound the denominator (double accuracy)
    const double fprop = 1.0 - 1.0 / std::exp(dexp);
    const double ftotal = 1.0 + fprop;
    f += fadd / fsecs * fprop;
    f /= ftotal;
    if (f < 2e-16) f = 0.0; // avoid meaningless sub-epsilon values
}

// ckpool src/stratifier.c time_bias(): corrects a decaying average that has not
// yet had `period` seconds to warm up, so early estimates aren't biased low.
double time_bias(double tdiff, double period) {
    double dexp = tdiff / period;
    if (dexp > 36.0) dexp = 36.0;
    return 1.0 - 1.0 / std::exp(dexp);
}

double secs_since(std::chrono::steady_clock::time_point a,
                  std::chrono::steady_clock::time_point b) {
    double s = std::chrono::duration<double>(a - b).count();
    return s < 0.001 ? 0.001 : s; // ckpool sane_tdiff() floor
}

} // namespace

VarDiff::VarDiff(VarDiffConfig cfg, double initial_difficulty)
    : cfg_(cfg),
      current_diff_(std::clamp(std::round(initial_difficulty), cfg.min_difficulty, cfg.max_difficulty)) {}

std::optional<double> VarDiff::on_share(std::chrono::steady_clock::time_point now,
                                        double observed_share_diff,
                                        double network_diff) {
    using clock = std::chrono::steady_clock;

    // First share of the session: seed timestamps and skip (one share spans no
    // measurable interval, so it carries no rate information yet).
    if (first_share_ts_ == clock::time_point{}) {
        first_share_ts_ = now;
        last_share_ts_ = now;
        last_diff_change_ts_ = now;
        ssdc_ = 1;
        return std::nullopt;
    }

    // Update the decaying hashrate estimates with this share, exactly as ckpool
    // does on every share regardless of whether we end up retargeting.
    const double decay_dt = secs_since(now, last_share_ts_);
    last_share_ts_ = now;
    decay_time(dsps1_, observed_share_diff, decay_dt, kFastWindowSeconds);
    decay_time(dsps5_, observed_share_diff, decay_dt, kSlowWindowSeconds);

    ++ssdc_;
    const double bdiff = secs_since(now, first_share_ts_);          // session age (bias)
    const double tdiff = secs_since(now, last_diff_change_ts_);     // since last change

    // Throttle: only reconsider every 240s, or once we've seen as many shares as
    // the target rate should have produced in that time, whichever comes first.
    if (ssdc_ < kSharesPerCheck && tdiff < kCheckSeconds)
        return std::nullopt;

    // A share whose difficulty no longer matches the active difficulty belongs
    // to a previous epoch (diff was changed mid-flight); reset and wait.
    if (observed_share_diff != current_diff_) {
        ssdc_ = 0;
        return std::nullopt;
    }

    // If shares are coming fast, use the 1-minute average for quick settling;
    // otherwise the smoother 5-minute average. Correct for warm-up via the bias.
    double dsps;
    if (ssdc_ >= kSharesPerCheck) {
        dsps = dsps1_ / time_bias(bdiff, kFastWindowSeconds);
    } else {
        dsps = dsps5_ / time_bias(bdiff, kSlowWindowSeconds);
    }

    const double drr = dsps / current_diff_; // observed shares/second
    if (drr > kDrrLow && drr < kDrrHigh)
        return std::nullopt; // within hysteresis band, leave difficulty alone

    double optimal = std::round(dsps * kOptimalMultiplier);

    optimal = std::max(optimal, cfg_.min_difficulty);
    if (cfg_.max_difficulty > 0.0) optimal = std::min(optimal, cfg_.max_difficulty);
    if (network_diff > 0.0)        optimal = std::min(optimal, network_diff);

    if (optimal < 1.0) return std::nullopt;
    if (optimal == current_diff_) return std::nullopt;

    // If the only reason to drop is a single share after a long idle gap, don't:
    // the client may have just returned and is about to resume its real rate.
    if (optimal < current_diff_ && ssdc_ == 1) {
        last_diff_change_ts_ = now;
        return std::nullopt;
    }

    ssdc_ = 0;
    last_diff_change_ts_ = now;
    current_diff_ = optimal;
    return current_diff_;
}

} // namespace mkpool
