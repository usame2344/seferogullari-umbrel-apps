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
// File:        client_session.hpp
// Description: Per-miner Stratum session interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "any_stream.hpp"
#include "bitcoin_client.hpp"
#include "stratifier.hpp"
#include "vardiff.hpp"
#include "write_queue.hpp"
#include "rate_limiter.hpp"
#include "sv2_messages.hpp"
#include "sv2_noise.hpp"

namespace mkpool {

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    using SocketT = AnyStream;
    using Strand  = boost::asio::strand<boost::asio::any_io_executor>;
    using DisconnectHandler = std::function<void(std::shared_ptr<ClientSession>)>;

    // `tls_ctx` non-null makes this a TLS-terminating session (Stratum V1 only).
    // The shared_ptr is retained for the session's lifetime so the OpenSSL
    // context outlives the stream even across a SIGHUP certificate reload.
    ClientSession(boost::asio::io_context& io,
                  const CoinConfig& coin,
                  StratifierPtr strat,
                  std::shared_ptr<bitcoin::BitcoinClient> btc,
                  std::shared_ptr<RateLimiter> rl,
                  std::shared_ptr<bitcoin::BitcoinClient> auxBtc = nullptr,
                  std::shared_ptr<boost::asio::ssl::context> tls_ctx = nullptr);

    // The raw TCP socket, used by the Connector for accept/endpoint/close.
    boost::asio::ip::tcp::socket& socket() { return socket_.lowest_layer(); }
    const std::string& client_ip() const { return client_ip_; }
    const std::string& worker_address() const { return worker_address_; }
    const std::string& worker_name() const { return worker_name_; }
    [[nodiscard]] const CoinConfig& coin() const { return coin_; }

    // --- Control plane ------------------------------------------
    // Per-connection IDENTITY only. These change at most a couple of times in a
    // session's life (subscribe/authorize), so the snapshot is (re)published on
    // those low-frequency transitions - never on the per-job broadcast path.
    // The control server reads it locklessly. Fast-moving numbers (shares,
    // hashrate, difficulty, last-share time) are NOT here: they are the live
    // atomics below, read on demand, so 50k idle workers cost nothing per job.
    struct StatSnapshot {
        std::string   ip;
        std::string   worker_address;
        std::string   worker_name;
        std::string   user_agent;
        std::string   protocol;        // sv1 / sv1+tls / sv2 / sv2+noise
        int           miner_id{-1};
        std::int64_t  connected_unix{0};
        std::uint16_t tier_port{0};
        bool          authorized{false};
        bool          subscribed{false};
        bool          is_v2{false};
        bool          is_tls{false};
    };
    [[nodiscard]] std::shared_ptr<const StatSnapshot> statSnapshot() const { return stat_snap_.load(); }

    // Live per-connection numerics, read by the control server on demand (never
    // published on the hot path). Written on this session's strand with relaxed
    // atomics on the session's own cache lines (no cross-session contention).
    [[nodiscard]] std::uint64_t liveSharesAccepted() const noexcept { return shares_accepted_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t liveSharesRejected() const noexcept { return shares_rejected_.load(std::memory_order_relaxed); }
    [[nodiscard]] double        liveDifficulty()     const noexcept { return stat_diff_.load(std::memory_order_relaxed); }
    [[nodiscard]] double        liveHashrate1m()     const noexcept { return stat_hr1_.load(std::memory_order_relaxed); }
    [[nodiscard]] double        liveHashrate5m()     const noexcept { return stat_hr5_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::int64_t  liveLastShareUnix()  const noexcept { return last_share_unix_.load(std::memory_order_relaxed); }

    // Send a Stratum V1 client.reconnect. Posts to the strand; a no-op for
    // Stratum V2 sessions. host/port empty -> reconnect to the same server;
    // wait = seconds to wait before reconnecting.
    void requestReconnect(std::string host, std::string port, int wait);

    // Reset this session's accepted/rejected/total counters (control-plane
    // "resetshares"). Posts to the strand and republishes the snapshot.
    void resetShareCounters();

    // Shares->hashes multiplier for this session's chain: hashrate estimate
    // = diff-shares/sec * this factor. SHA-256d = 2^32, Scrypt = 2^16, Equihash
    // uses a small basis to match the diff-1 target scoring.
    [[nodiscard]] double hashrate_multiplier() const noexcept;

    // Begin reading. Must be called once after socket is connected.
    void start();

    // Safe to call from any thread; posts shutdown to strand.
    void shutdown();

    // Broadcast hook: PoolManager calls this from arbitrary threads.
    // Posts message build & write to this session's strand.
    void notifyNewJob(JobPtr job);

    void setDisconnectHandler(DisconnectHandler h) { disconnect_handler_ = std::move(h); }

    // Strand executor, exposed for cross-strand posting (notify broadcasts).
    Strand& strand() { return strand_; }

private:
    void do_read();
    void on_read(const boost::system::error_code& ec, std::size_t bytes);
    void process_line(std::string_view line);
    void send_line(std::string msg);

    // Rebuild the control-plane snapshot from current strand-local state and
    // publish it locklessly. Called on the strand at authorize, diff change and
    // each job broadcast (never on the per-share hot path).
    void publish_snapshot();

    // record an accepted share for idle-tracking + pool best-share. Cheap
    // (one timestamp store + a lock-free atomic-max); no allocation, no publish.
    void note_accepted_share(double share_diff);

    void on_disconnect_local(const char* reason);

    // Stratum method handlers.
    void handle_configure(const nlohmann::json& msg);
    void handle_subscribe(const nlohmann::json& msg);
    void handle_authorize(const nlohmann::json& msg);
    void handle_submit(const nlohmann::json& msg);
    void handle_suggest_difficulty(const nlohmann::json& msg);

    void send_set_difficulty(double d, std::optional<double> old_diff = std::nullopt);
    void send_set_version_mask(std::uint32_t mask);
    void emit_job(const MiningJob& job, bool force_clean = false);
    void emit_job_custom(const MiningJob& job, const std::string& custom_prefix, bool force_clean = false);

    // Coinbase rebuild (solo style: per-session miner address).
    [[nodiscard]] std::string build_session_coinbase2(const MiningJob& job,
                                                     std::string_view miner_script_hex) const;

    // Assemble and submit a full block for a share that met the job's own
    // network target, then mark it precious so our node prefers it in a
    // same-height race. Shared by the accepted-share path and the stale /
    // low-diff rescue paths: a solve is submitted regardless of share status
    // and the node adjudicates validity.
    void submit_block_candidate(const JobPtr& job, const std::string& worker,
                                const std::string& header_hex,
                                const std::string& cb_hex,
                                const std::string& solution_s,
                                const std::string& block_hash_hex,
                                double share_diff);

    // DOGE merge mining: test the parent PoW hash against the aux target and
    // submit the self-assembled DOGE block if it wins. Also called for shares
    // whose parent job went stale: auxpow does not require the parent header
    // to be a valid LTC block, so a stale parent share can still win the
    // current DOGE block.
    void maybe_submit_aux_block(const JobPtr& job, const std::string& jobId,
                                const std::string& worker,
                                const std::string& header_hex,
                                const std::string& cb_hex,
                                const std::array<std::uint8_t, 32>& header_hash_be,
                                double share_diff);

    // Wire-protocol label for this connection: "sv1", "sv1+tls", "sv2",
    // "sv2+noise"; "+ext" appended for SV2 extended channels.
    [[nodiscard]] std::string protocol_label() const {
        if (!is_v2_) return is_tls_ ? "sv1+tls" : "sv1";
        std::string p = is_v2_encrypted_ ? "sv2+noise" : "sv2";
        if (is_extended_channel_) p += "+ext";
        return p;
    }

    // ------------------------------------------------------------------
    boost::asio::io_context& io_;
    Strand    strand_;
    // Declared before socket_ so the OpenSSL context (referenced by the TLS
    // stream inside socket_) is destroyed *after* the stream.
    std::shared_ptr<boost::asio::ssl::context> tls_ctx_;
    bool      is_tls_{false};
    SocketT   socket_;
    WriteQueue<SocketT> writes_;
    // Per-connection transport read chunk, kept small on purpose. Stratum V1 is
    // line-based - partial lines accumulate in buffer_ (capped at 1 MiB in
    // on_read), so any read size is correct - and SV2 reads are length-clamped
    // to this buffer and loop in do_read_v2, so a smaller buffer stays correct
    // for both protocols. The only hard floor is the 64-byte Noise act1 read.
    // For the line-based hot path (tiny submits) a small buffer costs nothing;
    // only rare large SV2 frames do a few extra read cycles. Shrinking from
    // 16 KiB is the single biggest idle-connection RAM win: this array is
    // inlined per session, so at 100k idle connections it is the dominant cost.
    std::array<char, 4096> read_buf_{};
    std::string buffer_;

    const CoinConfig& coin_;
    StratifierPtr     strat_;
    std::shared_ptr<bitcoin::BitcoinClient> btc_;
    std::shared_ptr<RateLimiter> rl_;
    std::shared_ptr<bitcoin::BitcoinClient> auxBtc_{nullptr};

    // Identity
    std::string client_ip_;
    std::string extranonce1_;
    std::string session_id_;
    std::string worker_address_;
    std::string worker_name_;
    std::string worker_script_hex_;   // pre-built scriptPubKey for miner LTC address
    std::string aux_doge_address_;    // custom DOGE address parsed from password
    std::string doge_script_hex_;     // pre-built scriptPubKey for miner DOGE address
    // Per-job self-assembled DOGE work. Built synchronously when a job is
    // broadcast (no createauxblock RPC), and consumed on a winning share to
    // submit a full DOGE block via submitblock.
    struct SessionAuxJob {
        std::string  aux_target;     // big-endian hex (from DOGE GBT)
        std::string  aux_commit;     // 88-hex merged-mining commitment spliced into cb1
        std::string  doge_coinbase;  // full DOGE coinbase tx hex
        std::string  doge_header;    // 80-byte DOGE header hex (auxpow-flag version, nonce 0)
        std::string  doge_block_hash;// display-order DOGE block hash (for logging/dedup)
        std::int32_t aux_height{0};
        std::int64_t aux_value{0};   // DOGE coinbase value (reward) for this aux block
    };
    std::unordered_map<std::string, SessionAuxJob> session_aux_jobs_;
    std::unordered_set<std::string> doge_submitted_;  // dedup DOGE block submissions

    // Per-job Zcash work: the node-built coinbase with its miner-reward output
    // redirected to this worker's t-address (funding streams preserved), plus
    // the recomputed (ZIP-244) merkle root. Built when a job is emitted, used
    // to assemble the block on a win. Empty -> falls back to node's payout.
    struct ZcashJob { std::string coinbase_hex; std::string merkle_root_le; };
    std::unordered_map<std::string, ZcashJob> zcash_jobs_;
    [[nodiscard]] std::optional<SessionAuxJob> build_session_aux(const MiningJob& job) const;
    std::string user_agent_;          // mining.subscribe params[0] ("NiceHash/1.0.0", "Antminer S19...")
    int         miner_id_{-1};

    // State
    bool subscribed_{false};
    bool authorized_{false};
    bool extranonce_subscribed_{false};
    bool tracked_in_metrics_{false};
    std::size_t extranonce2_size_{8};

    // BIP310
    bool          version_rolling_{false};
    std::uint32_t version_mask_{0};

    // Vardiff
    VarDiff       vardiff_;
    double        last_difficulty_{1.0};
    std::chrono::steady_clock::time_point last_difficulty_change_ts_{std::chrono::steady_clock::now()};
    bool          has_custom_difficulty_{false};
    double        custom_difficulty_{1024.0};

    // Job tracking. Atomic so the control server can read them live (relaxed;
    // written only on this strand, so no ordering needed - each is on the
    // session's own cache line, so no cross-session contention at scale).
    std::uint64_t              shares_total_{0};   // strand-only bookkeeping
    std::atomic<std::uint64_t> shares_accepted_{0};
    std::atomic<std::uint64_t> shares_rejected_{0};

    // Live control-plane numerics (written on the strand, read on demand).
    std::atomic<double>       stat_diff_{0.0};   // last advertised difficulty
    std::atomic<double>       stat_hr1_{0.0};    // 1-min hashrate estimate (H/s)
    std::atomic<double>       stat_hr5_{0.0};    // 5-min hashrate estimate (H/s)
    std::atomic<std::int64_t> last_share_unix_{0}; // wall-clock of last accepted share

    // Cleanup
    std::chrono::steady_clock::time_point connected_at_{std::chrono::steady_clock::now()};
    std::int64_t connected_unix_{0};   // wall-clock connect time for the control plane
    std::atomic<bool> closed_{false};
    DisconnectHandler disconnect_handler_;

    // Control-plane IDENTITY snapshot. Lock-free publication: written on the
    // strand at subscribe/authorize only, read by the control server thread.
    // Null until the first publish.
    std::atomic<std::shared_ptr<const StatSnapshot>> stat_snap_{};

    // Stratum V2 State & Logic
    bool is_v2_{false};
    bool is_v2_encrypted_{false};
    bool v2_handshake_done_{false};
    bool is_extended_channel_{false};
    sv2::NoiseState noise_state_;
    std::vector<uint8_t> sv2_read_buffer_;
    uint32_t sv2_channel_id_{0};
    uint32_t sv2_custom_job_id_counter_{0};

    struct CustomJob {
        uint32_t job_id{0};
        uint32_t version{0};
        std::array<uint8_t, 32> prev_hash{};
        std::vector<uint8_t> coinbase_prefix;
        std::vector<uint8_t> coinbase_outputs;
        uint32_t coinbase_tx_version{1};
        uint32_t coinbase_tx_input_nSequence{0xFFFFFFFFu};
        uint32_t coinbase_tx_locktime{0};
        uint32_t nbits{0};
    };
    std::map<uint32_t, CustomJob> sv2_custom_jobs_;
    std::optional<sv2::Header> sv2_pending_header_;  // Cached decrypted header awaiting payload

    void do_read_v2(std::size_t bytes_to_read);
    void process_v2_buffer();
    void process_message_v2(const sv2::Header& h, const std::vector<uint8_t>& payload);
    void send_v2_message(uint16_t ext_type, uint8_t msg_type, const sv2::Writer& w);
    void send_v2_error(const std::string& err_code);
    void send_v2_open_channel_error(uint32_t request_id, const std::string& err_code);
    void validate_payout_async();
    void emit_job_v2(const MiningJob& job);
    void handle_submit_shares_v2(const sv2::SubmitSharesStandard& msg);
    void handle_submit_shares_extended_v2(const sv2::SubmitSharesExtended& msg);
    // Assemble + submit the full block for a Stratum V2 share that meets the
    // network target. Callers gate on strat_->register_share() first-sighting so
    // a replayed share cannot double-submit. empty_block mirrors the standard
    // channel empty-block opt-in (extended channels always mine full blocks).
    void submit_block_sv2(const JobPtr& job, const std::string& header_hex,
                          const std::string& cb_hex,
                          const std::array<std::uint8_t, 32>& header_hash_be,
                          double share_diff, bool empty_block);
    void handle_set_custom_mining_job_v2(const sv2::SetCustomMiningJob& msg);

    std::array<uint8_t, 32> diff_to_target_bytes(double diff) const;
    void send_set_target(double d, std::optional<double> old_diff = std::nullopt);
};

} // namespace mkpool
