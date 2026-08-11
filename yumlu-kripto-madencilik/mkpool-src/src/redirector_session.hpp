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
// File:        redirector_session.hpp
// Description: Downstream session for the redirector role (client.reconnect steering).
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>

#include "config.hpp"
#include "relay_session.hpp"

namespace mkpool {

// Shared, thread-safe backend selector for a coin's redirector listeners. One
// instance is shared by every RedirectorSession of that coin so the rotation is
// global (not per-connection). Supports weighted round-robin, random, and
// latency-aware selection. When health checking is enabled it runs a background
// prober that TCP-connects each backend on an interval and records up/down +
// latency, and next() then only returns healthy backends (fastest first under
// strategy="latency"); it fails open (returns any target) if all are down.
class RedirectorPolicy : public std::enable_shared_from_this<RedirectorPolicy> {
public:
    RedirectorPolicy(boost::asio::io_context& io, const RedirectConfig& cfg);
    struct Pick { std::string host; std::uint16_t port; };
    [[nodiscard]] Pick next();
    [[nodiscard]] std::uint16_t wait() const noexcept { return wait_; }
    [[nodiscard]] bool empty() const noexcept { return targets_.empty(); }

    void start();   // begin health probing (no-op unless healthCheck is enabled)
    void stop();

    // Snapshot of backend health for the control socket.
    struct HealthRow { std::string host; std::uint16_t port; bool up; double latency_ms; };
    [[nodiscard]] std::vector<HealthRow> health() const;

private:
    struct Target {
        std::string host;
        std::uint16_t port{0};
        std::uint32_t weight{1};
        std::atomic<bool>   up{true};        // assumed up until a probe says otherwise
        std::atomic<double> latency_ms{0.0};
    };
    void schedule_probe();
    void probe_all();

    boost::asio::io_context& io_;
    std::vector<std::unique_ptr<Target>> targets_;
    std::vector<int> wheel_;              // weight-expanded target indices (round-robin order)
    std::string strategy_;
    std::uint16_t wait_{0};
    bool health_enabled_{false};
    int interval_s_{10};
    int timeout_ms_{1000};
    std::atomic<std::size_t> cursor_{0};
    boost::asio::steady_timer timer_;
    std::atomic<bool> running_{false};
};
using RedirectorPolicyPtr = std::shared_ptr<RedirectorPolicy>;

// Accepts a miner, answers its subscribe, then issues a Stratum V1
// client.reconnect steering it to a chosen backend, and closes shortly after.
// Constructed only when global.role == Redirector.
class RedirectorSession final : public RelaySession {
public:
    RedirectorSession(boost::asio::io_context& io,
                      std::shared_ptr<RateLimiter> rl,
                      RedirectorPolicyPtr policy,
                      std::shared_ptr<boost::asio::ssl::context> tls_ctx);

protected:
    [[nodiscard]] const char* role_label() const noexcept override { return "redirector"; }
    void on_line(std::string_view line) override;

private:
    void redirect_now();

    RedirectorPolicyPtr policy_;
    boost::asio::steady_timer close_timer_;
    bool redirected_{false};
    std::string session_id_;
};

} // namespace mkpool
