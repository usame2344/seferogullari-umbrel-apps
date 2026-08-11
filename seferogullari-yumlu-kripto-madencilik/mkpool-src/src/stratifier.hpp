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
// File:        stratifier.hpp
// Description: Workbase/job management interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "config.hpp"
#include "job_window.hpp"

namespace mkpool {

struct BlockTemplateTx {
    std::string data;     // tx hex
    std::string txid_le;  // little-endian (stratum display) txid hex
    std::int64_t fee{0};
    std::int32_t sigops{0};
    std::int32_t weight{0};
};

struct BlockTemplate {
    std::int32_t version{0};
    std::string  previousblockhash;
    std::int64_t coinbasevalue{0};
    std::string  target;
    std::uint32_t mintime{0};
    std::uint32_t curtime{0};
    std::string  bits;
    std::int32_t height{0};
    std::string  default_witness_commitment; // hex, may be empty
    std::string  reserved;                   // Zcash specific
    std::string  zcash_merkle_root;
    std::string  zcash_coinbase_data;
    bool         segwit_active{true};
    std::vector<BlockTemplateTx> transactions;

    // eCash specific consensus parameters from GBT
    std::string  miner_fund_address;
    std::int64_t miner_fund_value{0};
    std::string  staking_rewards_script;
    std::int64_t staking_rewards_value{0};

    // eCash Real-Time Target (RTT) inputs from GBT "rtt" (XEC only; otherwise
    // unset). Receive times of the previous blocks at depths {1,2,5,11,17} and
    // the previous block's compact nBits, used to reject blocks avalanche would
    // park (policy-bad-rtt). See utils::ecash_rtt_ok.
    bool                        rtt_present{false};
    std::array<std::uint32_t,5> rtt_prevheadertime{};
    std::uint32_t               rtt_prevbits{0};

    // Aux merged mining: full self-assembled DOGE (getblocktemplate) fields.
    // The aux block hash is NOT here - it is derived per-session because the
    // DOGE coinbase pays the miner's own DOGE address.
    bool          aux_enabled{false};
    std::uint32_t aux_version{0};       // GBT version | AUXPOW flag (0x100)
    std::string   aux_prevhash;         // display (big-endian) hex
    std::string   aux_bits;             // display hex
    std::uint32_t aux_curtime{0};
    std::int32_t  aux_height{0};
    std::int64_t  aux_coinbasevalue{0};
    std::string   aux_target;           // big-endian hex (64 chars)

    // Litecoin MWEB extension block data
    std::string  mweb;
};

// Per-broadcast job (immutable once published).
struct MiningJob {
    std::string  job_id;
    std::string  previousblockhash;
    std::string  coinbase1;
    std::vector<std::string> merkle_branch_le; // stratum LE hex
    std::uint32_t version{0};
    std::string  bits;
    std::uint32_t curtime{0};
    std::uint32_t mintime{0};
    bool          clean_jobs{false};

    // Used by ClientSession to rebuild a per-session coinbase2 in solo mode,
    // and for block reconstruction on a winning share.
    std::int64_t coinbase_value{0};
    std::int64_t total_fees{0};                // sum of template tx fees; used only by the v2 empty-block coinbase
    std::int32_t height{0};
    std::string  witness_commitment;
    std::string  reserved;                     // Zcash specific
    std::string  zcash_merkle_root_le;         // Zcash: pre-computed merkle root (pool-side)
    std::string  zcash_full_coinbase_hex;       // Zcash: full coinbase tx for block assembly
    bool         segwit_active{true};
    int          extranonce1_bytes{8};
    int          extranonce2_bytes{8};
    double       donation_percent{0.0};
    std::string  donation_address;

    // eCash specific consensus parameters from GBT
    std::string  miner_fund_address;
    std::int64_t miner_fund_value{0};
    std::string  staking_rewards_script;
    std::int64_t staking_rewards_value{0};

    // eCash Real-Time Target (RTT) inputs from GBT "rtt" (XEC only; otherwise
    // unset). Consumed by the submit path to withhold blocks that avalanche
    // would park as policy-bad-rtt (guaranteed orphan). See utils::ecash_rtt_ok.
    bool                        rtt_present{false};
    std::array<std::uint32_t,5> rtt_prevheadertime{};
    std::uint32_t               rtt_prevbits{0};

    // Full transaction hex list (for assembling the block on a network win).
    std::vector<std::string> tx_hexes;

    // Aux merged mining: self-assembled DOGE template (shared across miners).
    // Per-miner coinbase, merkle root and aux block hash are derived in the
    // ClientSession from these fields plus the miner's DOGE payout address.
    bool          aux_enabled{false};
    std::uint32_t aux_version{0};
    std::string   aux_prevhash;
    std::string   aux_bits;
    std::uint32_t aux_curtime{0};
    std::int32_t  aux_height{0};
    std::int64_t  aux_coinbasevalue{0};
    std::string   aux_target;

    // Litecoin MWEB extension block data
    std::string  mweb;

    // Pre-formatted job-common parts of the mining.notify frame. The per-
    // session emit only does `notify_prefix + cb2 + notify_suffix`, which is
    // an O(1) memcpy chain instead of a 9-arg fmt::format. Critical when
    // broadcasting a new job to tens of thousands of sessions in parallel.
    //
    // Wire layout:
    //   prefix = {"id":null,"method":"mining.notify","params":["<jobid>","<prevhash_stratum>","<cb1>","
    //   <session cb2 inserted here>
    //   suffix = ",["b1","b2",...],"<version_hex>","<bits>","<curtime_hex>",<clean>]}\n
    std::string notify_prefix;
    std::string notify_suffix;
};

using JobPtr = std::shared_ptr<const MiningJob>;
using WorkUpdateCallback = std::function<void(JobPtr)>;

class Stratifier {
public:
    explicit Stratifier(std::uint16_t job_window_size = 32);

    void setCoinConfig(const CoinConfig& cc);
    void registerCallback(WorkUpdateCallback cb);
    void updateWork(const nlohmann::json& gbt);

    [[nodiscard]] JobPtr latestJob() const { return jobs_.latest().value_or(nullptr); }
    [[nodiscard]] JobPtr findJob(std::string_view id) const { return jobs_.find(id).value_or(nullptr); }
    // Look up a recent job by the 32-bit id Stratum V2 sends on the wire.
    [[nodiscard]] JobPtr findJobU32(std::uint32_t id) const { return jobs_.find_u32(id).value_or(nullptr); }
    [[nodiscard]] std::uint32_t pool_version_mask() const noexcept { return version_mask_.load(); }

    // Current chain tip the pool is building on (display/big-endian hex, as
    // stored in MiningJob::previousblockhash). Used by the submit path to
    // reject stale shares whose job predates the last block change.
    [[nodiscard]] std::string current_prevhash() const;

    // Duplicate-share guard.
    // Records a share fingerprint (its header hash) and returns false if it was
    // already seen since the last block change. Global across sessions; the
    // table is purged whenever the tip advances (a share for a superseded
    // prevhash can never win the new block).
    [[nodiscard]] bool register_share(const std::string& fingerprint);

    // Pool best-share-of-round tracking. note_share_diff() records the
    // highest accepted share difficulty since the last block; it is reset when
    // the tip advances (in updateWork) and by the control-plane "resetshares".
    // Lock-free atomic max, safe to call from any session strand.
    void note_share_diff(double d) noexcept;
    [[nodiscard]] double best_share_round() const noexcept { return best_share_round_.load(std::memory_order_relaxed); }
    void reset_best_share() noexcept { best_share_round_.store(0.0, std::memory_order_relaxed); }

private:
    static std::optional<BlockTemplate> parseTemplate(const nlohmann::json& gbt);

    mutable std::mutex mu_;
    CoinConfig coin_{};
    std::vector<WorkUpdateCallback> callbacks_;
    JobWindow<JobPtr> jobs_;
    std::string last_prevhash_;
    std::atomic<std::uint32_t> version_mask_{0x1fffe000u};
    std::atomic<double> best_share_round_{0.0};

    // Duplicate-share table, guarded by its own mutex to keep the hot submit
    // path off the job-generation lock (mu_). Cleared on every block change.
    mutable std::mutex share_mu_;
    std::unordered_set<std::string> seen_shares_;
};

using StratifierPtr = std::shared_ptr<Stratifier>;

} // namespace mkpool
