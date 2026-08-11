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
// File:        stratifier.cpp
// Description: Workbase/job management and share difficulty-target computation.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "stratifier.hpp"

#include "address.hpp"
#include "merkle.hpp"
#include "utils.hpp"
#include "metrics.hpp"

#include <chrono>
#include <spdlog/spdlog.h>

namespace mkpool {

Stratifier::Stratifier(std::uint16_t window) : jobs_(window) {}

void Stratifier::setCoinConfig(const CoinConfig& cc) {
    std::lock_guard lk(mu_);
    coin_ = cc;
    version_mask_.store(cc.enableVersionRolling ? cc.versionRollingMask : 0);
}

void Stratifier::registerCallback(WorkUpdateCallback cb) {
    std::lock_guard lk(mu_);
    callbacks_.push_back(std::move(cb));
}

std::optional<BlockTemplate> Stratifier::parseTemplate(const nlohmann::json& gbt) {
    if (!gbt.contains("result") || !gbt["result"].is_object()) return std::nullopt;
    const auto& r = gbt["result"];
    BlockTemplate bt;
    bt.version            = r.value("version", 0);
    bt.previousblockhash  = r.value("previousblockhash", "");
    bt.coinbasevalue      = r.value("coinbasevalue", std::int64_t{0});
    bt.target             = r.value("target", "");
    bt.mintime            = r.value("mintime", std::uint32_t{0});
    bt.curtime            = r.value("curtime", std::uint32_t{0});
    bt.bits               = r.value("bits", "");
    bt.height             = r.value("height", 0);
    bt.default_witness_commitment = r.value("default_witness_commitment", "");
    bt.reserved           = r.value("reserved", "");

    if (r.contains("defaultroots") && r["defaultroots"].is_object()) {
        bt.zcash_merkle_root = r["defaultroots"].value("merkleroot", "");
        bt.reserved = r["defaultroots"].value("blockcommitmentshash", "");
    }
    if (r.contains("coinbasetxn") && r["coinbasetxn"].is_object()) {
        bt.zcash_coinbase_data = r["coinbasetxn"].value("data", "");
    }

    // Segwit is active iff the node provided a default_witness_commitment in
    // the GBT result (BTC post-activation). BCH never returns this field, so
    // BCH templates will correctly resolve to segwit_active=false and no
    // witness commitment / witness-reserved value will be emitted.
    bt.segwit_active = !bt.default_witness_commitment.empty();

    // Parse eCash Miner Fund / Staking Rewards from GBT response if present
    if (r.contains("coinbasetxn") && r["coinbasetxn"].is_object()) {
        const auto& cb = r["coinbasetxn"];
        if (cb.contains("minerfund") && cb["minerfund"].is_object()) {
            const auto& mf = cb["minerfund"];
            bt.miner_fund_value = mf.value("minimumvalue", std::int64_t{0});
            if (mf.contains("addresses") && mf["addresses"].is_array() && !mf["addresses"].empty()) {
                bt.miner_fund_address = mf["addresses"][0].get<std::string>();
            }
        }
        if (cb.contains("stakingrewards") && cb["stakingrewards"].is_object()) {
            const auto& sr = cb["stakingrewards"];
            bt.staking_rewards_value = sr.value("minimumvalue", std::int64_t{0});
            if (sr.contains("payoutscript") && sr["payoutscript"].is_object()) {
                bt.staking_rewards_script = sr["payoutscript"].value("hex", "");
            }
        }
    }

    // eCash Real-Time Target (RTT): the node supplies the previous-block receive
    // times (depths {1,2,5,11,17}) and prevbits needed to compute the real-time
    // target avalanche enforces. Only eCash's GBT carries "rtt"; other chains
    // never populate this, so rtt_present stays false and the submit path keeps
    // its DAA-only behaviour. Zero entries mean the node lacks the 17-block
    // history to compute RTT -> leave unset (fail safe, don't gate).
    if (r.contains("rtt") && r["rtt"].is_object()) {
        const auto& rtt = r["rtt"];
        if (rtt.contains("prevheadertime") && rtt["prevheadertime"].is_array() &&
            rtt["prevheadertime"].size() >= 5 && rtt.contains("prevbits")) {
            bool ok = true;
            std::array<std::uint32_t,5> pht{};
            for (int i = 0; i < 5; ++i) {
                const auto& v = rtt["prevheadertime"][i];
                if (!v.is_number()) { ok = false; break; }
                const std::int64_t t = v.get<std::int64_t>();
                if (t <= 0) { ok = false; break; }
                pht[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(t);
            }
            const std::string pb = rtt.value("prevbits", "");
            if (ok && pb.size() == 8 && utils::valid_hex(pb)) {
                bt.rtt_prevheadertime = pht;
                bt.rtt_prevbits       = static_cast<std::uint32_t>(std::stoul(pb, nullptr, 16));
                bt.rtt_present        = true;
            }
        }
    }

    if (r.contains("transactions") && r["transactions"].is_array()) {
        bt.transactions.reserve(r["transactions"].size());
        for (const auto& jt : r["transactions"]) {
            BlockTemplateTx tx;
            tx.data    = jt.value("data", "");
            // RPC's "txid" is in display form (byte-reversed of the raw SHA
            // output). For merkle math we need the internal/raw form, so
            // reverse here.
            // Bitcoin-family GBT provides "txid" (the non-witness id used for the
            // merkle tree). Zcash GBT omits "txid" and provides "hash" instead;
            // fall back to it only when "txid" is absent so segwit chains (where
            // "hash" is the wtxid) are unaffected.
            std::string txid_display = jt.value("txid", jt.value("hash", ""));
            tx.txid_le = utils::byte_reverse_hex(txid_display);
            tx.fee     = jt.value("fee",    std::int64_t{0});
            tx.sigops  = jt.value("sigops", 0);
            tx.weight  = jt.value("weight", 0);
            if (tx.txid_le.empty()) continue;
            bt.transactions.push_back(std::move(tx));
        }
    }
    if (r.contains("aux") && r["aux"].is_object()) {
        // Self-assembled DOGE merge mining: r["aux"] is the DOGE getblocktemplate
        // result. We build the DOGE coinbase/block ourselves so we never touch
        // the node's single-slot createauxblock cache.
        const auto& a = r["aux"];
        bt.aux_enabled       = true;
        bt.aux_version       = static_cast<std::uint32_t>(a.value("version", 0)) | 0x100u; // AUXPOW flag
        bt.aux_prevhash      = a.value("previousblockhash", "");
        bt.aux_bits          = a.value("bits", "");
        bt.aux_curtime       = static_cast<std::uint32_t>(a.value("curtime", 0));
        bt.aux_height        = static_cast<std::int32_t>(a.value("height", 0));
        bt.aux_coinbasevalue = a.value("coinbasevalue", std::int64_t{0});
        bt.aux_target        = a.value("target", "");
    }
    bt.mweb = r.value("mweb", "");
    return bt;
}

void Stratifier::updateWork(const nlohmann::json& gbt) {
    auto t0 = std::chrono::steady_clock::now();

    auto parsed = parseTemplate(gbt);
    if (!parsed) {
        spdlog::error("[Stratifier] parse failed");
        return;
    }
    BlockTemplate bt = std::move(*parsed);

    CoinConfig cc;
    {
        std::lock_guard lk(mu_);
        cc = coin_;
    }

    if (cc.donationAddress.empty()) {
        spdlog::error("[Stratifier] donationAddress not configured; refusing to build coinbase");
        return;
    }

    // ---------- coinbase1 (shared) ----------
    // Upgraded extranonce2 to 8 bytes (making total 12 bytes) to satisfy
    // Braiins OS+ / V2 Proxy requirements and prevent search-space starvation.
    const int en1 = 4;
    const int en2 = (cc.chain == ChainKind::Zcash) ? 4 : 8;
    std::string height_push = utils::encode_bip34_height(bt.height);
    std::string sig_push    = cc.coinbaseSignature.empty()
        ? std::string{} : utils::encode_push_ascii(cc.coinbaseSignature);

    // Reserve a fixed-length merged-mining commitment placeholder so the
    // scriptSig length byte is correct. Each ClientSession splices its own
    // "fabe6d6d" + <DOGE block hash> + merkle-size(1) + nonce(0) over the
    // trailing 88 hex chars, since the DOGE coinbase (hence its hash) is
    // per-miner. 44 bytes: 4 (magic) + 32 (hash) + 4 (size) + 4 (nonce).
    std::string aux_commit;
    if (bt.aux_enabled && !bt.aux_prevhash.empty()) {
        aux_commit = "fabe6d6d" + std::string(64, '0') + "01000000" + "00000000";
    }

    std::size_t scriptSigLen = height_push.size()/2 + sig_push.size()/2 + en1 + en2;
    if (!aux_commit.empty()) {
        scriptSigLen += aux_commit.size() / 2;
    }
    if (scriptSigLen > 0xfc) {
        spdlog::error("[Stratifier] scriptSig too long ({} bytes)", scriptSigLen);
        return;
    }
    char lenbuf[3];
    std::snprintf(lenbuf, sizeof(lenbuf), "%02zx", scriptSigLen);

    std::string coinbase1;
    if (cc.chain == ChainKind::Zcash) {
        coinbase1 = bt.zcash_coinbase_data;
    } else {
        coinbase1.reserve(160 + aux_commit.size());
        coinbase1 += utils::le_hex_u32(1);   // tx version
        coinbase1 += "01";                    // input count
        coinbase1 += std::string(64, '0');    // prev txid
        coinbase1 += "ffffffff";              // prev vout
        coinbase1 += lenbuf;                  // scriptSig length
        coinbase1 += height_push;
        coinbase1 += sig_push;
        coinbase1 += aux_commit;
    }
    // Miner appends extranonce1 || extranonce2 || coinbase2

    // ---------- merkle branch ----------
    std::vector<std::string> txid_le;
    txid_le.reserve(bt.transactions.size());
    for (const auto& tx : bt.transactions) txid_le.push_back(tx.txid_le);
    std::vector<std::string> branch;
    try {
        branch = merkle::build_branch_le_hex(txid_le);
    } catch (const std::exception& e) {
        spdlog::error("[Stratifier] merkle branch error: {}", e.what());
        return;
    }

    // ---------- assemble MiningJob ----------
    MiningJob mj;
    if (coin_.chain == ChainKind::Zcash) {
        mj.job_id          = utils::generate_job_id_32();
    } else {
        mj.job_id          = utils::generate_job_id();
    }
    mj.previousblockhash   = bt.previousblockhash;
    mj.coinbase1           = std::move(coinbase1);
    mj.merkle_branch_le    = std::move(branch);
    mj.version             = std::uint32_t(bt.version);
    mj.bits                = bt.bits;
    mj.curtime             = bt.curtime;
    mj.mintime             = bt.mintime;
    mj.coinbase_value      = bt.coinbasevalue;
    mj.total_fees          = 0;
    for (const auto& tx : bt.transactions) mj.total_fees += tx.fee;
    mj.height              = bt.height;
    mj.witness_commitment  = bt.default_witness_commitment;
    mj.reserved            = bt.reserved;
    mj.segwit_active       = bt.segwit_active;
    mj.extranonce1_bytes   = en1;
    mj.extranonce2_bytes   = en2;
    mj.donation_percent    = cc.donationPercent;
    mj.donation_address    = cc.donationAddress;

    // eCash specific consensus parameters from GBT
    mj.miner_fund_address   = bt.miner_fund_address;
    mj.miner_fund_value     = bt.miner_fund_value;
    mj.staking_rewards_script = bt.staking_rewards_script;
    mj.staking_rewards_value = bt.staking_rewards_value;

    mj.rtt_present          = bt.rtt_present;
    mj.rtt_prevheadertime   = bt.rtt_prevheadertime;
    mj.rtt_prevbits         = bt.rtt_prevbits;

    mj.aux_enabled          = bt.aux_enabled;
    mj.aux_version          = bt.aux_version;
    mj.aux_prevhash         = bt.aux_prevhash;
    mj.aux_bits             = bt.aux_bits;
    mj.aux_curtime          = bt.aux_curtime;
    mj.aux_height           = bt.aux_height;
    mj.aux_coinbasevalue    = bt.aux_coinbasevalue;
    mj.aux_target           = bt.aux_target;
    mj.mweb                 = bt.mweb;

    mj.tx_hexes.reserve(bt.transactions.size());
    for (auto& tx : bt.transactions) mj.tx_hexes.push_back(std::move(tx.data));

    bool clean = false;
    {
        std::lock_guard lk(mu_);
        clean = (mj.previousblockhash != last_prevhash_);
        last_prevhash_ = mj.previousblockhash;
    }
    mj.clean_jobs = clean;

    // Block change: drop the duplicate-share table. Shares from the previous
    // block can never solve the new one, so any fingerprint collision across
    // this boundary is irrelevant - and this keeps the table bounded to one
    // block's worth of work.
    if (clean) {
        std::lock_guard lk(share_mu_);
        seen_shares_.clear();
    }
    // A new block starts a fresh round for best-share accounting.
    if (clean) reset_best_share();

    // ---------- Zcash: pre-compute full coinbase + merkle root ----------
    // In Zcash stratum, the extranonce goes in the 32-byte nonce field of the
    // block header, NOT in the coinbase.  The pool sends a pre-computed merkle
    // root directly.  This block builds the complete coinbase tx, computes its
    // txid, and derives the merkle root.  Non-Zcash coins skip this entirely.
    if (cc.chain == ChainKind::Zcash) {
        mj.zcash_full_coinbase_hex = bt.zcash_coinbase_data;
        mj.zcash_merkle_root_le    = utils::byte_reverse_hex(bt.zcash_merkle_root);
        spdlog::info("[Stratifier] Zcash: merkle_root_le={}", mj.zcash_merkle_root_le.substr(0, 16));
    }

    // ---------- pre-format the job-common notify prefix/suffix ----------
    // ASIC firmware expects the prevhash with its 8 32-bit words REVERSED.
    // Compute it once here instead of per-session.
    std::string prevhash_stratum;
    if (cc.chain == ChainKind::Zcash) {
        prevhash_stratum = utils::byte_reverse_hex(mj.previousblockhash);
    } else {
        prevhash_stratum.resize(64);
        if (mj.previousblockhash.size() == 64) {
            for (std::size_t w = 0; w < 8; ++w) {
                std::memcpy(&prevhash_stratum[w * 8],
                            &mj.previousblockhash[(7 - w) * 8], 8);
            }
        } else {
            prevhash_stratum = mj.previousblockhash;
        }
    }
    std::string branch_json;
    branch_json.reserve(mj.merkle_branch_le.size() * 70);
    branch_json.push_back('[');
    for (std::size_t i = 0; i < mj.merkle_branch_le.size(); ++i) {
        if (i) branch_json.push_back(',');
        branch_json.push_back('"');
        branch_json.append(mj.merkle_branch_le[i]);
        branch_json.push_back('"');
    }
    branch_json.push_back(']');

    if (cc.chain == ChainKind::Zcash) {
        // Zcash 8-param notify:
        // [job_id, version_le, prevhash, merkle_root, reserved, time_le, bits_le, clean]
        std::string version_le = utils::le_hex_u32(mj.version);
        std::string reserved_hex = mj.reserved.empty()
            ? std::string(64, '0') : utils::byte_reverse_hex(mj.reserved);
        std::string time_le = utils::le_hex_u32(mj.curtime);
        std::string bits_le = utils::byte_reverse_hex(mj.bits);

        // No cb2 insertion point - the entire notify is pre-built.
        mj.notify_prefix = fmt::format(
            R"({{"id":null,"method":"mining.notify","params":["{}","{}","{}","{}","{}","{}","{}",{}]}})" "\n",
            mj.job_id, version_le, prevhash_stratum,
            mj.zcash_merkle_root_le, reserved_hex,
            time_le, bits_le, clean ? "true" : "false");
        mj.notify_suffix.clear();  // unused for Zcash
    } else {
        mj.notify_prefix = fmt::format(
            R"({{"id":null,"method":"mining.notify","params":["{}","{}","{}",")",
            mj.job_id, prevhash_stratum, mj.coinbase1);
        mj.notify_suffix = fmt::format(
            R"(",{},"{:08x}","{}","{:08x}",{}]}})" "\n",
            branch_json, mj.version, mj.bits, mj.curtime, clean ? "true" : "false");
    }

    auto job_ptr = std::make_shared<const MiningJob>(std::move(mj));
    jobs_.push(job_ptr->job_id, job_ptr);

    // Notify subscribers without holding the mutex.
    std::vector<WorkUpdateCallback> cbs;
    {
        std::lock_guard lk(mu_);
        cbs = callbacks_;
    }
    for (auto& cb : cbs) {
        try { if (cb) cb(job_ptr); }
        catch (const std::exception& e) {
            spdlog::error("[Stratifier] callback threw: {}", e.what());
        }
    }

    auto elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count();
    metrics::observe_template_build_us(elapsed);
    spdlog::info("[Stratifier] new job {} prevhash={}... branch_len={} txs={} (build {:.0f}us)",
                 job_ptr->job_id, job_ptr->previousblockhash.substr(0, 12),
                 job_ptr->merkle_branch_le.size(), job_ptr->tx_hexes.size(), elapsed);
}

std::string Stratifier::current_prevhash() const {
    std::lock_guard lk(mu_);
    return last_prevhash_;
}

bool Stratifier::register_share(const std::string& fingerprint) {
    std::lock_guard lk(share_mu_);
    // Safety cap so a pathological flood can't grow the table unbounded between
    // block changes. Clearing degrades to "a dup might be re-counted" only under
    // extreme load - it can never cause a wrongful reject of a real share.
    if (seen_shares_.size() > 1'000'000) seen_shares_.clear();
    return seen_shares_.insert(fingerprint).second;
}

// Lock-free atomic max of the best accepted share difficulty this round.
void Stratifier::note_share_diff(double d) noexcept {
    if (!(d > 0.0)) return;
    double cur = best_share_round_.load(std::memory_order_relaxed);
    while (d > cur &&
           !best_share_round_.compare_exchange_weak(cur, d, std::memory_order_relaxed)) {
        // cur reloaded by compare_exchange_weak on failure
    }
}

} // namespace mkpool
