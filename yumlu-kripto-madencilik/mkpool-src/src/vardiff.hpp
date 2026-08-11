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
// File:        vardiff.hpp
// Description: Variable-difficulty controller interface.
//
//              The decaying-average retarget algorithm re-implements ckpool's vardiff
//              (Con Kolivas, GPLv3, src/stratifier.c + src/libckpool.c); the surrounding
//              async engine and per-coin policy are original to mkpool.
// Created:     2026-06-02
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace mkpool {

    struct VarDiffConfig {
        // Retained for construction compatibility; the controller now follows
        // ckpool's fixed ~0.3 shares/s target rather than this value.
        double target_share_rate_hz{0.2};
        double min_difficulty{1.0};
        double max_difficulty{1e9};
        double tau_seconds{30.0};         // unused (ckpool uses fixed 60/300s windows)
    };

    class VarDiff {
    public:
        VarDiff(VarDiffConfig cfg, double initial_difficulty);

        // Returns the new difficulty value if a retarget should be sent, else
        // std::nullopt. Call from the session strand on every accepted share.
        //
        // `observed_share_diff` is the difficulty of the share that just landed
        //   (the active session difficulty). Shares whose diff no longer matches
        //   the current difficulty belong to a previous epoch and are skipped.
        // `network_diff` is the current coin/network difficulty; the retarget is
        //   capped to it (ckpool-style) so we never demand work harder than a
        //   block. Pass 0 to skip this clamp.
        [[nodiscard]] std::optional<double>
            on_share(std::chrono::steady_clock::time_point now,
                     double observed_share_diff = 0.0,
                     double network_diff = 0.0);

        [[nodiscard]] double current() const noexcept { return current_diff_; }

        // Decayed difficulty-shares-per-second, already maintained on every
        // accepted share. Multiplying by the coin's shares->hashes factor
        // yields a hashrate estimate at zero extra hot-path cost. Read from the
        // session strand (same thread as on_share()).
        [[nodiscard]] double dsps1() const noexcept { return dsps1_; }
        [[nodiscard]] double dsps5() const noexcept { return dsps5_; }
        void set_current(double diff) noexcept {
            current_diff_ = diff;
            ssdc_ = 0;
            first_share_ts_ = std::chrono::steady_clock::time_point{};
            last_share_ts_ = std::chrono::steady_clock::time_point{};
            last_diff_change_ts_ = std::chrono::steady_clock::time_point{};
        }

    private:
        VarDiffConfig cfg_;
        double current_diff_;
        double dsps1_{0.0};                                          // decaying diff-shares/s, 1 min
        double dsps5_{0.0};                                          // decaying diff-shares/s, 5 min
        std::chrono::steady_clock::time_point first_share_ts_{};     // session start (bias)
        std::chrono::steady_clock::time_point last_share_ts_{};      // last decay
        std::chrono::steady_clock::time_point last_diff_change_ts_{}; // ckpool "ldc"
        std::uint64_t ssdc_{0};                                       // shares since diff change
    };

} // namespace mkpool
