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
// File:        network_stats.cpp
// Description: Network difficulty, block and orphan statistics tracking.
// Created:     2026-06-02
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "network_stats.hpp"

#include "database.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

namespace mkpool {

namespace {

constexpr int    HALVING_INTERVAL = 210000;
constexpr double SUBSIDY_BTC      = 50.0;

double subsidy_for_height(const std::string& coin, long long height) {
    if (height < 0) return 0.0;
    
    std::string ticker = coin;
    for (auto& c : ticker) c = std::toupper(c);
    
    if (ticker == "BTC" || ticker == "BCH" || ticker == "BC2" || ticker == "BCH2") {
        const long long h = height / 210000;
        if (h >= 64) return 0.0;
        return 50.0 / static_cast<double>(1ULL << h);
    }
    else if (ticker == "XEC") {
        const long long h = height / 210000;
        if (h >= 64) return 0.0;
        double base_reward = 50000000.0 / static_cast<double>(1ULL << h);
        return base_reward * 0.58; // 58% goes to miner, 42% goes to development & staking funds
    }
    else if (ticker == "LTC") {
        const long long h = height / 840000;
        if (h >= 64) return 0.0;
        return 50.0 / static_cast<double>(1ULL << h);
    }
    else if (ticker == "DOGE") {
        return 10000.0;
    }
    else if (ticker == "ZEC") {
        // Zcash subsidy with the Blossom block-time adjustment (ZIP-208) plus the
        // post-Canopy 80/20 split (80% miner, 20% funding streams + lockbox).
        // Verified against zcashd getblocksubsidy at height 3,363,403:
        //   totalblocksubsidy 1.5625 ZEC, miner 1.25 ZEC.
        constexpr long long BLOSSOM_ACTIVATION    = 653600;   // mainnet
        constexpr long long SLOW_START_SHIFT      = 10000;    // nSubsidySlowStartInterval / 2
        constexpr long long PRE_BLOSSOM_HALVING   = 840000;
        constexpr long long POST_BLOSSOM_HALVING  = 1680000;  // doubled: block time halved
        constexpr long long BLOSSOM_RATIO         = 2;

        double base = 12.5;
        long long halvings;
        if (height >= BLOSSOM_ACTIVATION) {
            base /= BLOSSOM_RATIO;                            // 6.25 ZEC per (shorter) block
            const long long scaled =
                (BLOSSOM_ACTIVATION - SLOW_START_SHIFT) * BLOSSOM_RATIO
                + (height - BLOSSOM_ACTIVATION);
            halvings = scaled / POST_BLOSSOM_HALVING;
        } else {
            halvings = (height - SLOW_START_SHIFT) / PRE_BLOSSOM_HALVING;
        }
        if (halvings >= 64) return 0.0;
        const double total = base / static_cast<double>(1ULL << halvings);
        return total * 0.80;                                 // miner share after Canopy
    }
    else if (ticker == "DGB") {
        if (height < 1430000) {
            if (height < 1440) return 72000.0;
            if (height < 5760) return 16000.0;
            return 8000.0;
        } else {
            double reward = 1078.5;
            long long blocks = height - 1430000;
            long long months = (blocks * 15) / 2628000;
            for (long long i = 0; i < months; ++i) {
                reward = (reward * 98884.0) / 100000.0;
            }
            if (reward < 1.0) reward = 1.0;
            return reward;
        }
    }
    
    // Default fallback
    const long long h = height / 210000;
    if (h >= 64) return 0.0;
    return 50.0 / static_cast<double>(1ULL << h);
}

std::string compact_to_target_hex(std::uint32_t bits) {
    std::uint32_t mant = bits & 0x00FFFFFFu;
    std::uint32_t exp  = (bits >> 24) & 0xFFu;
    std::array<std::uint8_t, 32> t{};
    if (exp <= 3) {
        mant >>= 8u * (3u - exp);
        t[29] = static_cast<std::uint8_t>((mant >> 16) & 0xff);
        t[30] = static_cast<std::uint8_t>((mant >> 8)  & 0xff);
        t[31] = static_cast<std::uint8_t>( mant        & 0xff);
    } else if (exp <= 32) {
        const std::size_t off = 32u - exp;
        t[off + 0] = static_cast<std::uint8_t>((mant >> 16) & 0xff);
        if (off + 1 < 32) t[off + 1] = static_cast<std::uint8_t>((mant >> 8) & 0xff);
        if (off + 2 < 32) t[off + 2] = static_cast<std::uint8_t>( mant       & 0xff);
    }
    std::string s; s.reserve(64);
    for (std::uint8_t b : t) s += fmt::format("{:02x}", b);
    return s;
}

std::uint32_t parse_bits_hex(const std::string& hex) {
    if (hex.size() != 8) return 0;
    try { return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16)); }
    catch (...) { return 0; }
}

std::string iso8601_utc(std::int64_t unix_secs) {
    using namespace std::chrono;
    const auto tp = system_clock::time_point(seconds(unix_secs));
    const auto tt = system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << ".000Z";
    return os.str();
}

std::string network_type_from_chain(const std::string& chain) {
    if (chain == "main")    return "Main";
    if (chain == "test")    return "Testnet";
    if (chain == "test4")   return "Testnet";
    if (chain == "signet")  return "Signet";
    if (chain == "regtest") return "Regtest";
    return chain.empty() ? "Unknown" : chain;
}

} // anonymous

// ---------------------------------------------------------------------------

NetworkStatsTracker::NetworkStatsTracker(
    boost::asio::io_context& io,
    std::shared_ptr<bitcoin::BitcoinClient> btc,
    CoinConfig coin,
    std::chrono::seconds heartbeat)
    : io_(io),
      btc_(std::move(btc)),
      coin_(std::move(coin)),
      heartbeat_(heartbeat),
      timer_(io) {}

void NetworkStatsTracker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    spdlog::info("[NetStats] {} starting; heartbeat={}s", coin_.name, heartbeat_.count());
    // When an external feed (the Generator's single hashblock subscription)
    // drives us, we do NOT open our own ZMQ socket - this avoids a duplicate
    // subscription to the same node. Refreshes then come from notifyNewBlock()
    // plus the heartbeat fallback.
    if (external_block_feed_) {
        spdlog::info("[NetStats] {} using external block feed (no own ZMQ)", coin_.name);
    } else {
        start_zmq();
    }
    boost::asio::post(io_, [self = shared_from_this()] { self->refresh(); });
    schedule_heartbeat();
}

void NetworkStatsTracker::notifyNewBlock() {
    if (!running_.load()) return;
    boost::asio::post(io_, [self = shared_from_this()] { self->refresh(); });
}

void NetworkStatsTracker::stop() {
    running_.store(false);
    timer_.cancel();
    if (zmq_) zmq_->stop();
}

void NetworkStatsTracker::start_zmq() {
    const std::string ep = coin_.primary_zmq_hashblock();
    if (ep.empty()) {
        spdlog::warn("[NetStats] {} no ZMQ endpoint; heartbeat-only", coin_.name);
        return;
    }
    try {
        zmq_ = std::make_shared<ZMQClient>(io_, ep);
        zmq_->subscribe("hashblock");
        auto self = shared_from_this();
        zmq_->start([self](const std::string& topic, const std::string&) {
            if (topic == "hashblock" && self->running_.load()) {
                boost::asio::post(self->io_, [self] { self->refresh(); });
            }
        });
        spdlog::info("[NetStats] {} subscribed to hashblock @ {}", coin_.name, ep);
    } catch (const std::exception& e) {
        spdlog::warn("[NetStats] {} ZMQ setup failed: {}", coin_.name, e.what());
    }
}

void NetworkStatsTracker::schedule_heartbeat() {
    if (!running_.load()) return;
    timer_.expires_after(heartbeat_);
    auto self = shared_from_this();
    timer_.async_wait([self](const boost::system::error_code& ec) {
        if (ec || !self->running_.load()) return;
        self->refresh();
        self->schedule_heartbeat();
    });
}

void NetworkStatsTracker::refresh() {
    if (!running_.load()) return;
    bool expected = false;
    // Coalesce overlapping refreshes (ZMQ tick + heartbeat firing together).
    if (!in_flight_.compare_exchange_strong(expected, true)) return;

    auto self  = shared_from_this();
    auto state = std::make_shared<nlohmann::json>(nlohmann::json::object());

    btc_->asyncGetBlockchainInfo(
        [self, state](const boost::system::error_code& ec, const nlohmann::json& r) {
            if (ec || !r.contains("result") || r["result"].is_null()) {
                spdlog::warn("[NetStats] {} getblockchaininfo failed: {}",
                             self->coin_.name, ec ? ec.message() : "no result");
                self->in_flight_.store(false);
                return;
            }
            (*state)["chain_info"] = r["result"];
            const auto& ci = r["result"];
            if (!ci.contains("bestblockhash") || !ci["bestblockhash"].is_string()) {
                self->in_flight_.store(false);
                return;
            }
            const std::string best = ci["bestblockhash"].get<std::string>();

            self->btc_->asyncGetBlock(best,
                [self, state](const boost::system::error_code& ec2, const nlohmann::json& r2) {
                    if (ec2 || !r2.contains("result") || r2["result"].is_null()) {
                        spdlog::warn("[NetStats] {} getblock(best) failed", self->coin_.name);
                        self->in_flight_.store(false);
                        return;
                    }
                    (*state)["best_block"] = r2["result"];
                    const auto& bb = r2["result"];

                    auto finish_with_mining = [self, state] {
                        self->btc_->asyncGetMiningInfo(
                            [self, state](const boost::system::error_code&, const nlohmann::json& rm) {
                                if (rm.contains("result") && !rm["result"].is_null())
                                    (*state)["mining_info"] = rm["result"];
                                try {
                                    self->commit(*state);
                                } catch (const std::exception& e) {
                                    spdlog::error("[NetStats] {} commit failed: {}",
                                                  self->coin_.name, e.what());
                                }
                                self->in_flight_.store(false);
                            });
                    };

                    if (bb.contains("previousblockhash") && bb["previousblockhash"].is_string()) {
                        const std::string prev = bb["previousblockhash"].get<std::string>();
                        self->btc_->asyncGetBlock(prev,
                            [self, state, finish_with_mining](
                                const boost::system::error_code&, const nlohmann::json& rp) {
                                if (rp.contains("result") && !rp["result"].is_null())
                                    (*state)["prev_block"] = rp["result"];
                                finish_with_mining();
                            });
                    } else {
                        finish_with_mining();
                    }
                });
        });
}

void NetworkStatsTracker::commit(const nlohmann::json& state) {
    if (!state.contains("chain_info") || !state.contains("best_block")) return;

    const auto& ci = state["chain_info"];
    const auto& bb = state["best_block"];

    const std::string chain = ci.value("chain", std::string{});
    const std::string netType = network_type_from_chain(chain);

    const long long height = bb.value("height", 0LL);
    const long long btime  = bb.value("time", 0LL);
    const std::string bits_hex = bb.value("bits", std::string{"00000000"});
    const std::uint32_t bits_u = parse_bits_hex(bits_hex);
    const std::string next_target = compact_to_target_hex(bits_u);

    double difficulty = 0.0;
    std::string ticker = coin_.name;
    for (auto& c : ticker) c = std::toupper(c);

    if (ticker == "DGB") {
        if (ci.contains("difficulties") && ci["difficulties"].contains("sha256d")) {
            difficulty = ci["difficulties"]["sha256d"].get<double>();
        }
    }

    if (difficulty == 0.0) {
        difficulty = ci.value("difficulty", 0.0);
    }
    if (difficulty == 0.0) {
        difficulty = bb.value("difficulty", 0.0);
    }

    double networkhashps = 0.0;
    if (state.contains("mining_info")) {
        const auto& mi = state["mining_info"];
        if (ticker == "DGB" && mi.contains("networkhashesps") && mi["networkhashesps"].contains("sha256d")) {
            const auto& v = mi["networkhashesps"]["sha256d"];
            if (v.is_number()) networkhashps = v.get<double>();
            else if (v.is_string()) { try { networkhashps = std::stod(v.get<std::string>()); } catch (...) {} }
        }
        if (networkhashps == 0.0 && mi.contains("networkhashps")) {
            const auto& v = mi["networkhashps"];
            if      (v.is_number()) networkhashps = v.get<double>();
            else if (v.is_string()) { try { networkhashps = std::stod(v.get<std::string>()); } catch (...) {} }
        }
    }

    // Inter-block interval: prefer (best.time - prev.time); fallback to
    // best.mediantime delta if unavailable.
    int block_time = 0;
    if (state.contains("prev_block") && state["prev_block"].contains("time")) {
        const long long ptime = state["prev_block"].value("time", 0LL);
        if (btime > ptime) block_time = static_cast<int>(btime - ptime);
    }
    if (block_time <= 0) {
        // Reasonable network default if we can't compute.
        block_time = 600;
    }

    const std::string last_block_iso = iso8601_utc(btime);
    const double reward = subsidy_for_height(coin_.name, height);

    try {
        Database::execute(
            "INSERT INTO network_stats "
            "(coin, network_type, network_hashrate, network_difficulty, "
            " block_time, next_target, next_bits, last_block_time, "
            " block_height, block_reward, updated_at) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7,$8::timestamptz,$9,$10, now()) "
            "ON CONFLICT (coin) DO UPDATE SET "
            "  network_type=EXCLUDED.network_type, "
            "  network_hashrate=EXCLUDED.network_hashrate, "
            "  network_difficulty=EXCLUDED.network_difficulty, "
            "  block_time=EXCLUDED.block_time, "
            "  next_target=EXCLUDED.next_target, "
            "  next_bits=EXCLUDED.next_bits, "
            "  last_block_time=EXCLUDED.last_block_time, "
            "  block_height=EXCLUDED.block_height, "
            "  block_reward=EXCLUDED.block_reward, "
            "  updated_at=now()",
            coin_.name,
            netType,
            networkhashps,
            difficulty,
            block_time,
            next_target,
            bits_hex,
            last_block_iso,
            height,
            reward);

        Database::execute(
            "INSERT INTO network_difficulty_history (coin, difficulty, created_at) "
            "VALUES ($1, $2, now())",
            coin_.name,
            difficulty);

        spdlog::debug("[NetStats] {} height={} diff={:.3e} hps={:.3e} bt={}s",
                      coin_.name, height, difficulty, networkhashps, block_time);
    } catch (const std::exception& e) {
        spdlog::error("[NetStats] {} upsert failed: {}", coin_.name, e.what());
    }
}

} // namespace mkpool
