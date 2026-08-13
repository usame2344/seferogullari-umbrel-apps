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
// File:        db_worker.cpp
// Description: Asynchronous batched writer for shares, hashrates and miner presence.
// Created:     2026-06-05
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "db_worker.hpp"
#include "config.hpp"
#include "database.hpp"
#include "metrics.hpp"

#include <spdlog/spdlog.h>

namespace mkpool {

namespace {
std::unique_ptr<DbWorker> g_worker;
std::string g_conn_string;
}

void set_db_worker(std::unique_ptr<DbWorker> w) { g_worker = std::move(w); }
DbWorker& db_worker() {
    if (!g_worker) throw std::runtime_error("db_worker not initialized");
    return *g_worker;
}

// Internal: set by Database::init via a friend hook (declared in database.cpp)
namespace detail {
void set_db_conn_string(std::string s) { g_conn_string = std::move(s); }
}

// ---------------------------------------------------------------------------

DbWorker::DbWorker(Config cfg) : cfg_(cfg) {}

DbWorker::~DbWorker() {
    stop();
}

void DbWorker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    thread_ = std::jthread([this]{ run(); });
}

void DbWorker::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool DbWorker::enqueue_share(ShareRecord r) {
    {
        std::lock_guard lk(mu_);
        if (shares_.size() >= cfg_.max_queue) {
            shares_dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        shares_.push_back(std::move(r));
    }
    cv_.notify_one();
    return true;
}

bool DbWorker::enqueue_presence(PresenceRecord r) {
    {
        std::lock_guard lk(mu_);
        if (presences_.size() >= cfg_.max_queue) return false;
        presences_.push_back(std::move(r));
    }
    cv_.notify_one();
    return true;
}

bool DbWorker::enqueue_best_share(BestShareRecord r) {
    {
        std::lock_guard lk(mu_);
        if (best_shares_.size() >= cfg_.max_queue) return false;
        best_shares_.push_back(std::move(r));
    }
    cv_.notify_one();
    return true;
}

bool DbWorker::enqueue_hashrate(HashrateRecord r) {
    {
        std::lock_guard lk(mu_);
        if (hashrates_.size() >= cfg_.max_queue) return false;
        hashrates_.push_back(std::move(r));
    }
    cv_.notify_one();
    return true;
}

bool DbWorker::enqueue_block(BlockRecord r) {
    {
        std::lock_guard lk(mu_);
        // Blocks are precious; never drop them on queue pressure. The
        // queue is bounded by max_queue so the overall memory footprint
        // stays safe even if Postgres is unreachable for a long time.
        if (blocks_.size() >= cfg_.max_queue) {
            spdlog::error("[DbWorker] blocks queue saturated, dropping height={} hash={}",
                          r.height, r.block_hash);
            return false;
        }
        blocks_.push_back(std::move(r));
    }
    cv_.notify_one();
    return true;
}

DbWorker::Stats DbWorker::stats() const {
    Stats s;
    s.shares_written = shares_written_.load(std::memory_order_relaxed);
    s.shares_dropped = shares_dropped_.load(std::memory_order_relaxed);
    s.batches        = batches_.load(std::memory_order_relaxed);
    std::lock_guard lk(const_cast<std::mutex&>(mu_));
    s.queue_depth = shares_.size() + presences_.size() + best_shares_.size() + hashrates_.size() + blocks_.size();
    return s;
}

void DbWorker::run() {
    std::unique_ptr<pqxx::connection> conn;
    try {
        conn = std::make_unique<pqxx::connection>(g_conn_string);
        spdlog::info("[DbWorker] started");
    } catch (const std::exception& e) {
        // Postgres can be momentarily unavailable right when we start - e.g.
        // needrestart bounces both mkpool and Postgres together after a libssl
        // upgrade, and we lose the race. Do NOT give up: leaving the thread
        // dead silently stops all DB writes until a manual process restart.
        // The main loop below reconnects on its own; queued records are kept.
        spdlog::error("[DbWorker] initial connect failed, will retry in loop: {}", e.what());
    }

    auto reconnect = [&] {
        spdlog::warn("[DbWorker] reconnecting...");
        try {
            conn = std::make_unique<pqxx::connection>(g_conn_string);
        } catch (const std::exception& e) {
            spdlog::error("[DbWorker] reconnect failed: {}", e.what());
        }
    };
    auto last_hashrate_agg = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_acquire)) {
        // Ensure a live connection BEFORE draining the queue. If Postgres is
        // down (e.g. a needrestart bounce), pulling records out only to fail
        // would lose them - instead leave them queued, back off, and retry.
        // The enqueue side still caps the queue, so this can't grow unbounded.
        if (!conn || !conn->is_open()) {
            reconnect();
            if (!conn || !conn->is_open()) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
        }

        std::vector<ShareRecord>     shares_batch;
        std::vector<PresenceRecord>  presence_batch;
        std::vector<BestShareRecord> best_batch;
        std::vector<HashrateRecord>  hash_batch;
        std::vector<BlockRecord>     block_batch;

        {
            std::unique_lock lk(mu_);
            cv_.wait_for(lk, cfg_.flush_interval, [&]{
                return !running_.load(std::memory_order_acquire) ||
                       shares_.size()    >= cfg_.batch_size ||
                       presences_.size() >= cfg_.batch_size ||
                       best_shares_.size() >= cfg_.batch_size ||
                       hashrates_.size() >= cfg_.batch_size ||
                       !blocks_.empty();
            });
            shares_batch.reserve(shares_.size());
            std::move(shares_.begin(), shares_.end(), std::back_inserter(shares_batch));
            shares_.clear();
            presence_batch.reserve(presences_.size());
            std::move(presences_.begin(), presences_.end(), std::back_inserter(presence_batch));
            presences_.clear();
            best_batch.reserve(best_shares_.size());
            std::move(best_shares_.begin(), best_shares_.end(), std::back_inserter(best_batch));
            best_shares_.clear();
            hash_batch.reserve(hashrates_.size());
            std::move(hashrates_.begin(), hashrates_.end(), std::back_inserter(hash_batch));
            hashrates_.clear();
            block_batch.reserve(blocks_.size());
            std::move(blocks_.begin(), blocks_.end(), std::back_inserter(block_batch));
            blocks_.clear();
            metrics::set_db_queue_depth((std::int64_t)(shares_.size() + presences_.size() + best_shares_.size() + hashrates_.size() + blocks_.size()));
        }

        // Periodically run 5-minute hashrate aggregation from raw_shares to hashrates
        {
            auto now = std::chrono::steady_clock::now();
            if (now - last_hashrate_agg >= std::chrono::minutes(5)) {
                if (conn && conn->is_open()) {
                    try {
                        pqxx::work agg_txn(*conn);
                        agg_txn.exec(fmt::format(
                            "INSERT INTO hashrates (miner_id, hashrate, created_at) "
                            "SELECT s.miner_id, "
                            "       (COALESCE(SUM(s.difficulty), 0.0) * {:.1f}) / 300.0 AS hashrate, "
                            "       time_bucket('5 minutes'::interval, s.created_at) AS ts "
                            "FROM raw_shares s "
                            "WHERE s.created_at >= now() - interval '10 minutes' "
                            "  AND s.created_at < time_bucket('5 minutes'::interval, now()) "
                            "  AND NOT EXISTS ( "
                            "      SELECT 1 FROM hashrates h "
                            "      WHERE h.miner_id = s.miner_id "
                            "        AND h.created_at = time_bucket('5 minutes'::interval, s.created_at) "
                            "  ) "
                            "GROUP BY s.miner_id, ts",
                            cfg_.share_multiplier)
                        );
                        agg_txn.commit();
                        last_hashrate_agg = now;
                        spdlog::info("[DbWorker] Auto-aggregated 5-minute hashrates from raw_shares");
                    } catch (const std::exception& e) {
                        spdlog::error("[DbWorker] Hashrate auto-aggregation error: {}", e.what());
                    }
                }
            }
        }

        if (shares_batch.empty() && presence_batch.empty() &&
            best_batch.empty() && hash_batch.empty() && block_batch.empty())
            continue;

        // Connection was verified live at the top of the loop; if it dropped in
        // between, the flush throws broken_connection below and we requeue.
        try {
            pqxx::work txn(*conn);

            for (const auto& p : presence_batch) {
                auto pr = txn.exec(
                    "INSERT INTO miners (btc_address, worker_name, status, last_known_ip) "
                    "VALUES ($1,$2,$3::miner_status,$4) "
                    "ON CONFLICT (btc_address, worker_name) DO UPDATE "
                    "SET status = $3::miner_status, last_known_ip = $4, updated_at = now() "
                    "RETURNING id",
                    pqxx::params{p.btc_address, p.worker_name, p.status, p.ip});
                // Pre-warm the io-thread-facing miner-id cache so the hot
                // authorize path resolves without ever hitting the DB lock.
                if (!pr.empty() && !pr[0][0].is_null())
                    Database::miner_cache_put(p.btc_address, p.worker_name,
                                              pr[0][0].as<int>());
            }

            if (!shares_batch.empty()) {
                // Batched COPY would be faster; for now use multi-VALUES insert.
                // Build dynamic VALUES list.
                std::string sql =
                    "INSERT INTO raw_shares "
                    "(miner_id, job_id, nonce, extranonce1, extranonce2, share_hash, difficulty, accepted, block_found) VALUES ";
                std::vector<std::string> placeholders;
                placeholders.reserve(shares_batch.size());
                pqxx::params p;
                int idx = 1;
                for (const auto& s : shares_batch) {
                    placeholders.push_back(fmt::format(
                        "(${},${},${},${},${},${},${},${},${})",
                        idx, idx+1, idx+2, idx+3, idx+4, idx+5, idx+6, idx+7, idx+8));
                    idx += 9;
                    p.append(s.miner_id);
                    p.append(s.job_id);
                    p.append(s.nonce);
                    p.append(s.extranonce1);
                    p.append(s.extranonce2);
                    p.append(s.share_hash);
                    p.append(s.difficulty);
                    p.append(s.accepted);
                    p.append(s.block_found);
                }
                sql += fmt::format("{}", fmt::join(placeholders, ","));
                txn.exec(sql, p);
                shares_written_.fetch_add(shares_batch.size(), std::memory_order_relaxed);
            }

            // Durable effort accumulator: add this batch's accepted-share work to
            // the running total since the last block. The REST API reads it as
            // effort% = accum_diff / network_difficulty. Being a persistent running
            // sum (reset on block, below) it is independent of raw_shares' 6h
            // retention, so solo effort stays accurate even across long dry spells.
            {
                double batch_diff = 0.0;
                for (const auto& s : shares_batch)
                    if (s.accepted) batch_diff += s.difficulty;
                if (batch_diff > 0.0)
                    txn.exec(
                        "UPDATE effort_state SET accum_diff = accum_diff + $1, "
                        "updated_at = now() WHERE id = 1", pqxx::params{batch_diff});
            }

            for (const auto& b : best_batch) {
                txn.exec(
                    "UPDATE miners SET best_share_difficulty = $1, best_share_hash = $2, "
                    "last_share_at = now(), updated_at = now() "
                    "WHERE id = $3 AND best_share_difficulty < $1",
                    pqxx::params{b.difficulty, b.hash, b.miner_id});
            }

            for (const auto& h : hash_batch) {
                auto ts_t = std::chrono::system_clock::to_time_t(h.ts);
                std::tm tm_buf{};
                ::gmtime_r(&ts_t, &tm_buf);
                char ts_buf[32];
                std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
                txn.exec(
                    "INSERT INTO hashrates (miner_id, hashrate, created_at) VALUES ($1, $2, $3)",
                    pqxx::params{h.miner_id, h.hashrate_hps, std::string(ts_buf)});
            }

            // Network difficulty snapshot for PARENT blocks in this batch - frozen
            // so the REST API can show a stable pool effort% (round_effort /
            // net_difficulty). Filtered to this process's primary coin: the merged
            // LTC+DOGE DB holds a network_stats row per coin, so an LTC block must
            // snapshot LTC's difficulty (not whichever row updated most recently).
            double net_diff_snapshot = 0.0;
            if (!block_batch.empty()) {
                auto nd = txn.exec(
                    "SELECT COALESCE(network_difficulty,0)::float8 "
                    "FROM network_stats WHERE upper(coin) = upper($1) "
                    "ORDER BY updated_at DESC LIMIT 1",
                    pqxx::params{cfg_.coin});
                if (!nd.empty() && !nd[0][0].is_null())
                    net_diff_snapshot = nd[0][0].as<double>();
            }

            for (const auto& b : block_batch) {
                // A block whose coin differs from this process's primary coin is an
                // AUX (merge-mined) block - e.g. a DOGE block under the LTC pool. It
                // is persisted for history/chart but is PURELY ADDITIVE: it must NOT
                // touch the shared effort accumulator (which belongs to the parent
                // coin). So aux blocks get no round/finder effort snapshot and do
                // NOT reset effort_state. Parent blocks (coin empty or == primary)
                // drive effort exactly as before.
                const std::string block_coin = b.coin.empty() ? cfg_.coin : b.coin;
                const bool is_aux = (block_coin != cfg_.coin);

                double round_effort       = 0.0;
                double finder_effort      = 0.0;
                double net_diff_for_block = 0.0;

                if (!is_aux) {
                    net_diff_for_block = net_diff_snapshot;

                    // Snapshot the round's accumulated work BEFORE the reset below,
                    // so the effort% is frozen at found-time. The share-accumulation
                    // step above already folded this batch (incl. the winning share)
                    // into accum_diff, so this is the full round total.
                    {
                        auto er = txn.exec(
                            "SELECT COALESCE(accum_diff,0)::float8 FROM effort_state LIMIT 1");
                        if (!er.empty() && !er[0][0].is_null())
                            round_effort = er[0][0].as<double>();
                    }

                    // Finder's OWN accumulated work for this round (see 0003),
                    // mirroring the REST API's per-address "personal effort": sum the
                    // finder address's accepted share-difficulty since that address's
                    // PREVIOUS block. The "previous block" window is bounded to PARENT
                    // blocks of this coin (coin == primary, or legacy NULL) so it stays
                    // aligned with the effort accumulator - which only resets on parent
                    // blocks - and an interleaved aux (DOGE) block can't shrink it.
                    // Recent work (≤6h) from raw_shares (incl. this batch's winning
                    // share); older work from the durable hashrates rollup.
                    if (b.miner_id > 0) {
                        auto fe = txn.exec(
                            "WITH addr AS (SELECT btc_address AS a FROM miners WHERE id = $1), "
                            "since AS (SELECT COALESCE(MAX(b2.found_at), "
                            "                 '1970-01-01 00:00:00+00'::timestamptz) AS s "
                            "          FROM blocks b2 JOIN miners m2 ON m2.id = b2.found_by, addr "
                            "          WHERE m2.btc_address = addr.a "
                            "            AND (b2.coin IS NULL OR upper(b2.coin) = upper($2))) "
                            "SELECT ("
                            "  COALESCE((SELECT SUM(h.hashrate) * 300.0 / 4294967296.0 "
                            "            FROM hashrates h JOIN miners m ON m.id = h.miner_id, addr, since "
                            "            WHERE m.btc_address = addr.a "
                            "              AND h.created_at > since.s "
                            "              AND h.created_at <= now() - interval '6 hours'), 0.0) "
                            "+ COALESCE((SELECT SUM(rs.difficulty) "
                            "            FROM raw_shares rs JOIN miners m ON m.id = rs.miner_id, addr, since "
                            "            WHERE m.btc_address = addr.a AND rs.accepted "
                            "              AND rs.created_at > GREATEST(since.s, now() - interval '6 hours')), 0.0) "
                            ")::float8",
                            pqxx::params{b.miner_id, cfg_.coin});
                        if (!fe.empty() && !fe[0][0].is_null())
                            finder_effort = fe[0][0].as<double>();
                    }
                }

                // Use NULLIF for miner_id so a missing miner row (b.miner_id<=0)
                // becomes SQL NULL rather than triggering an FK violation.
                // ON CONFLICT(block_hash) DO NOTHING handles double-submits.
                txn.exec(
                    "INSERT INTO blocks (height, block_hash, found_by, difficulty, reward_value, round_effort, net_difficulty, finder_effort, coin) "
                    "VALUES ($1, $2, NULLIF($3, 0), $4, $5, $6, $7, $8, $9) "
                    "ON CONFLICT (block_hash) DO NOTHING",
                    pqxx::params{b.height, b.block_hash,
                    b.miner_id > 0 ? b.miner_id : 0,
                    b.difficulty, b.reward_value,
                    round_effort, net_diff_for_block, finder_effort, block_coin});
                spdlog::info("[DbWorker] persisted block height={} hash={} coin={}{}",
                             b.height, b.block_hash, block_coin, is_aux ? " (aux)" : "");

                // Solved PARENT block: reset the effort accumulator. (Runs after the
                // share accumulation above, so a batch containing the winning share
                // nets to zero - effort restarts from the new block.) Aux blocks do
                // not own the accumulator, so they leave it untouched.
                if (!is_aux) {
                    txn.exec(
                        "UPDATE effort_state SET accum_diff = 0, last_block_height = $1, "
                        "last_reset = now(), updated_at = now() WHERE id = 1", pqxx::params{b.height});
                }
            }

            txn.commit();
            batches_.fetch_add(1, std::memory_order_relaxed);
        } catch (const pqxx::broken_connection& e) {
            // The connection died mid-flush; the transaction never committed, so
            // nothing was written. Put the whole batch back at the FRONT of each
            // queue (reverse-iterate so intra-batch order is preserved) and let
            // the top of the loop reconnect. This is what makes a DB bounce
            // lossless instead of costing one batch of shares.
            spdlog::error("[DbWorker] broken connection during flush, requeuing "
                          "{} shares / {} blocks: {}",
                          shares_batch.size(), block_batch.size(), e.what());
            {
                std::lock_guard lk(mu_);
                for (auto it = shares_batch.rbegin(); it != shares_batch.rend(); ++it)
                    shares_.push_front(std::move(*it));
                for (auto it = presence_batch.rbegin(); it != presence_batch.rend(); ++it)
                    presences_.push_front(std::move(*it));
                for (auto it = best_batch.rbegin(); it != best_batch.rend(); ++it)
                    best_shares_.push_front(std::move(*it));
                for (auto it = hash_batch.rbegin(); it != hash_batch.rend(); ++it)
                    hashrates_.push_front(std::move(*it));
                for (auto it = block_batch.rbegin(); it != block_batch.rend(); ++it)
                    blocks_.push_front(std::move(*it));
            }
            conn.reset();  // force a fresh connect at the top of the next loop
        } catch (const std::exception& e) {
            // Non-connection error (e.g. a malformed record). Requeuing would
            // loop forever on the poison batch, so log and drop this batch only.
            spdlog::error("[DbWorker] flush error (batch dropped): {}", e.what());
        }
    }

    spdlog::info("[DbWorker] exiting");
}

} // namespace mkpool
