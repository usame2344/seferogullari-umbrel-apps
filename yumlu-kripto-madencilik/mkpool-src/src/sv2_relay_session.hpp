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
// File:        sv2_relay_session.hpp
// Description: Stratum V2 (Noise) downstream session for the proxy role - SV2->V1 translation.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "config.hpp"
#include "sv2_noise.hpp"
#include "sv2_messages.hpp"
#include "upstream_client.hpp"

namespace mkpool {

class RateLimiter;

// A Stratum V2 (Noise-encrypted) miner connected to the proxy. It accepts the
// SV2 Noise handshake + a standard mining channel, but its work comes from the
// shared V1 UpstreamClient: the upstream V1 mining.notify is translated into an
// SV2 NewMiningJob + SetNewPrevHash (server computes the merkle root), and the
// miner's SubmitSharesStandard is translated back into a V1 mining.submit with
// the reconstructed extranonce2. This lets an SV2 miner (e.g. a Bitaxe) mine
// against ANY plain-Stratum-V1 upstream pool. Only constructed in the proxy role
// for a tier flagged sv2:true, so it never touches the solo path.
//
// Standalone (not a RelaySession): SV2 is a binary, Noise-framed protocol, not
// the newline-delimited V1 the relay base handles.
class Sv2RelaySession final : public std::enable_shared_from_this<Sv2RelaySession>,
                              public ProxyWorkSink {
public:
    using Strand            = boost::asio::strand<boost::asio::any_io_executor>;
    using DisconnectHandler = std::function<void(const std::shared_ptr<Sv2RelaySession>&)>;

    Sv2RelaySession(boost::asio::io_context& io,
                    const CoinConfig& coin,
                    std::shared_ptr<RateLimiter> rl,
                    std::shared_ptr<UpstreamSource> upstream,
                    std::string authority_key_hex);

    boost::asio::ip::tcp::socket& socket() { return socket_; }
    [[nodiscard]] const std::string& client_ip() const { return client_ip_; }

    void start();
    void shutdown();  // safe from any thread
    void setDisconnectHandler(DisconnectHandler h) { disconnect_handler_ = std::move(h); }

    // ---- ProxyWorkSink (called by UpstreamClient from its strand; each posts to
    // this session's strand) ----
    void on_upstream_job(ProxyNotifyPtr job) override;
    void on_upstream_diff(double diff) override;
    void on_upstream_extranonce(std::string en1_hex, std::size_t en2_size) override;
    void on_upstream_state(bool up) override;
    void on_rebind(ProxySlot slot, ProxyNotifyPtr job, double diff) override;

private:
    std::shared_ptr<ProxyWorkSink> sink_ptr();

    // --- transport (raw TCP + Noise) ---
    void do_read();
    void on_read(const boost::system::error_code& ec, std::size_t n);
    void read_handshake();                      // read the 64-byte act1
    void consume_frames();                       // parse buffered noise frames
    void send_sv2(std::uint16_t ext, std::uint8_t type, const sv2::Writer& payload);
    void raw_send(std::vector<std::uint8_t> bytes);
    void do_write();
    void close(const char* reason);

    // --- SV2 message handlers (on strand) ---
    void handle_frame(const sv2::Header& h, sv2::Reader& r);
    void handle_setup_connection(sv2::Reader& r);
    void handle_open_standard_channel(sv2::Reader& r);
    void handle_submit_shares(sv2::Reader& r);

    // --- job / share translation ---
    void try_attach();                           // reserve an upstream slot once ready
    void emit_job(const ProxyNotifyPtr& job, bool clean);
    void send_set_target(double diff);
    [[nodiscard]] std::string current_fixed_en2() const;

    boost::asio::io_context& io_;
    Strand    strand_;
    boost::asio::ip::tcp::socket socket_;
    const CoinConfig& coin_;
    std::shared_ptr<RateLimiter> rl_;
    std::shared_ptr<UpstreamSource> upstream_;

    sv2::NoiseState noise_;
    std::string authority_key_hex_;
    bool handshake_done_{false};
    bool noise_active_{false};   // true => Noise-encrypted transport; false => plaintext SV2

    std::string client_ip_;
    std::atomic<bool> closed_{false};
    DisconnectHandler disconnect_handler_;

    // read side
    std::array<std::uint8_t, 8192> read_buf_{};
    std::vector<std::uint8_t> in_;               // accumulated ciphertext
    // framed decode state machine
    bool have_header_{false};
    sv2::Header cur_header_{};

    // write side (raw bytes; Noise-encrypted frames or handshake)
    std::deque<std::vector<std::uint8_t>> wq_;
    bool writing_{false};

    // SV2 channel state
    bool     setup_done_{false};
    bool     channel_open_{false};
    std::uint32_t channel_id_{1};
    std::uint32_t open_request_id_{0};
    bool     want_version_rolling_{false};
    std::string user_identity_;

    // upstream binding
    ProxySlot slot_;
    bool      attached_{false};
    double    target_diff_{1.0};

    // job bookkeeping: SV2 job_id -> (upstream ProxyNotify, fixed extranonce2 hex)
    struct JobRec { ProxyNotifyPtr up; std::string fixed_en2_hex; };
    std::unordered_map<std::uint32_t, JobRec> jobs_;
    std::deque<std::uint32_t> job_order_;
    std::uint32_t next_job_id_{1};
    std::uint64_t en2_counter_{0};               // rotates the fixed en2 per job

    ProxyNotifyPtr latest_up_job_;
    std::uint64_t  shares_forwarded_{0}, shares_accepted_{0}, shares_rejected_{0};
};

class IoPool;

// Accepts Stratum V2 miners for the proxy role and hands each to an
// Sv2RelaySession bound to the shared V1 UpstreamClient. A standalone sibling of
// RelayListener (SV2 sessions are not RelaySessions), so the solo/V1 paths are
// untouched. Only started for a proxy coin that declares an sv2 downstream tier.
class Sv2RelayListener : public std::enable_shared_from_this<Sv2RelayListener> {
public:
    Sv2RelayListener(boost::asio::io_context& accept_io,
                     IoPool& workers,
                     std::string bind_addr,
                     std::uint16_t port,
                     std::shared_ptr<RateLimiter> rl,
                     const CoinConfig& coin,
                     std::shared_ptr<UpstreamSource> upstream,
                     std::string authority_key_hex,
                     std::string label);

    void start();
    void stop();

private:
    void do_accept();

    boost::asio::io_context& accept_io_;
    IoPool& workers_;
    std::string bind_addr_;
    std::uint16_t port_;
    std::shared_ptr<RateLimiter> rl_;
    const CoinConfig& coin_;
    std::shared_ptr<UpstreamSource> upstream_;
    std::string authority_key_hex_;
    std::string label_;

    boost::asio::ip::tcp::acceptor acceptor_;
    std::atomic<bool> running_{false};
};

} // namespace mkpool
