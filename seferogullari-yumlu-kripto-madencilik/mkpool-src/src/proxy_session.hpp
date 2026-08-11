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
// File:        proxy_session.hpp
// Description: Downstream miner session for the proxy role.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "relay_session.hpp"
#include "upstream_client.hpp"

namespace mkpool {

// A downstream miner connected to a proxy. It speaks ordinary Stratum V1 (+TLS)
// but its work comes from the shared UpstreamClient rather than a local
// Stratifier: on subscribe it is given an extranonce slice carved from the
// upstream extranonce space, it relays the upstream mining.notify verbatim, and
// on submit it locally scores the share (reusing the same utils/merkle used by
// the solo path) and forwards qualifying shares upstream with the reconstructed
// extranonce2. No coinbase is built here and no block is submitted locally - the
// upstream pool owns payout and block submission. This class is only ever
// constructed when global.role == Proxy, so it cannot affect the solo path.
class ProxyDownstreamSession final : public RelaySession, public ProxyWorkSink {
public:
    // `coin` must outlive the session (PoolManager keeps a stable per-tier copy,
    // exactly like the solo ClientSession).
    ProxyDownstreamSession(boost::asio::io_context& io,
                           const CoinConfig& coin,
                           std::shared_ptr<RateLimiter> rl,
                           std::shared_ptr<UpstreamSource> upstream,
                           std::shared_ptr<boost::asio::ssl::context> tls_ctx);

    // ProxyWorkSink (called by UpstreamClient from its strand; each posts to
    // this session's strand).
    void on_upstream_job(ProxyNotifyPtr job) override;
    void on_upstream_diff(double diff) override;
    void on_upstream_extranonce(std::string en1_hex, std::size_t en2_size) override;
    void on_upstream_state(bool up) override;
    void on_rebind(ProxySlot slot, ProxyNotifyPtr job, double diff) override;

protected:
    [[nodiscard]] const char* role_label() const noexcept override { return "proxy"; }
    void on_line(std::string_view line) override;
    void on_closed() override;

private:
    std::shared_ptr<ProxyWorkSink> sink_ptr();

    // Stratum handlers (run on the strand).
    void handle_configure(const nlohmann::json& msg);
    void handle_subscribe(const nlohmann::json& msg);
    void handle_authorize(const nlohmann::json& msg);
    void handle_submit(const nlohmann::json& msg);

    void try_attach_and_reply();       // allocate slot + send subscribe reply
    void send_subscribe_reply();
    void send_set_difficulty(double d);
    void emit_current_job(bool clean);
    void remember_job(const ProxyNotifyPtr& job);

    const CoinConfig& coin_;
    std::shared_ptr<UpstreamSource> upstream_;

    // Downstream identity/extranonce.
    ProxySlot   slot_;
    bool        attached_{false};
    std::string session_id_;
    std::string user_agent_;
    std::string worker_name_;

    // Negotiated state.
    bool          subscribed_{false};
    bool          authorized_{false};
    bool          extranonce_subscribed_{false};
    bool          version_rolling_{false};
    std::uint32_t version_mask_{0};

    // A subscribe that arrived before the uplink was ready waits for readiness.
    bool        pending_subscribe_{false};
    std::string pending_subscribe_id_;

    // Difficulty. upstream_diff_ is the forward threshold; target_diff_ is what
    // we advertise + accept at downstream (mirrors upstream by default).
    double upstream_diff_{0.0};
    double target_diff_{0.0};

    // Recent upstream jobs, so a submit for the just-previous job still validates.
    std::unordered_map<std::string, ProxyNotifyPtr> recent_jobs_;
    std::deque<std::string> recent_order_;
    ProxyNotifyPtr cur_job_;

    std::uint64_t shares_accepted_{0};
    std::uint64_t shares_rejected_{0};
    std::uint64_t shares_forwarded_{0};
};

} // namespace mkpool
