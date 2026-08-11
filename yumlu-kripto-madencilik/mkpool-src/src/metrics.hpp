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
// File:        metrics.hpp
// Description: Prometheus metrics interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace mkpool::metrics {

    void init(const std::string& bind_addr, std::uint16_t port);
    void shutdown();

    // Counters
    void inc_share_accepted();
    void inc_share_rejected(const char* reason);
    void inc_share_stale();
    void inc_share_invalid();
    void inc_block_found();
    void inc_connections_accepted();
    void inc_connections_rejected(const char* reason);

    // Gauges
    void set_connections_open(std::int64_t value);
    void set_active_miners(std::int64_t value);
    void set_db_queue_depth(std::int64_t value);

    // Histograms (microseconds)
    void observe_share_processing_us(double value);
    void observe_template_build_us(double value);

} // namespace mkpool::metrics
