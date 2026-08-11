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
// File:        database.cpp
// Description: PostgreSQL/TimescaleDB schema initialisation and connection management.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "database.hpp"
#include "config.hpp"

namespace mkpool {

void Database::init(const Config& config) {
    if (connection_) return;
    const auto& g = config.global;

    std::string conn_str = fmt::format(
        "host={} port={} dbname={} user={} password={}",
        g.databaseHost, g.databasePort, g.databaseName,
        g.databaseUser, g.databasePassword);

    conn_str_ = conn_str;  // kept so conn() can transparently reconnect later.
    connection_ = std::make_unique<pqxx::connection>(conn_str);
    if (!connection_->is_open())
        throw std::runtime_error("Database connection failed");

    // Share conn string with DbWorker (it makes its own connection).
    detail::set_db_conn_string(conn_str);

    spdlog::info("[DB] connected user={} db={} host={}:{}",
                 g.databaseUser, g.databaseName, g.databaseHost, g.databasePort);

    // ---------- Enum ----------
    execute(R"SQL(
        DO $$
        BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_type WHERE typname = 'miner_status') THEN
                CREATE TYPE miner_status AS ENUM ('online', 'offline', 'banned');
            END IF;
        END$$;
    )SQL");

    // ---------- miners ----------
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS miners (
            id SERIAL PRIMARY KEY,
            btc_address TEXT NOT NULL,
            worker_name TEXT NOT NULL,
            status miner_status DEFAULT 'online',
            best_share_difficulty DOUBLE PRECISION DEFAULT 0.0,
            best_share_hash TEXT,
            last_share_at TIMESTAMPTZ,
            last_known_ip INET,
            created_at TIMESTAMPTZ DEFAULT now(),
            updated_at TIMESTAMPTZ DEFAULT now(),
            UNIQUE (btc_address, worker_name)
        )
    )SQL");
    ensureIndex("idx_miners_btc",    "CREATE INDEX idx_miners_btc ON miners(btc_address)");
    ensureIndex("idx_miners_worker", "CREATE INDEX idx_miners_worker ON miners(worker_name)");

    // ---------- raw_shares ----------
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS raw_shares (
            miner_id INTEGER NOT NULL REFERENCES miners(id) ON DELETE CASCADE,
            job_id TEXT NOT NULL,
            nonce TEXT NOT NULL,
            extranonce1 TEXT NOT NULL,
            extranonce2 TEXT NOT NULL,
            share_hash TEXT NOT NULL,
            difficulty DOUBLE PRECISION NOT NULL,
            accepted BOOLEAN NOT NULL,
            block_found BOOLEAN DEFAULT false,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
    )SQL");
    ensureHypertable("raw_shares", "created_at");
    // Small chunks so the 6h retention below can actually prune: the default 7-day
    // chunk_time_interval means a chunk only drops once its whole 7-day span ages
    // past 6h, so raw_shares would balloon to ~7 days. 1h chunks keep it at ~6-7h.
    execute("SELECT set_chunk_time_interval('raw_shares', INTERVAL '1 hour')");
    ensureIndex("idx_raw_shares_miner_ts",
                "CREATE INDEX idx_raw_shares_miner_ts ON raw_shares(miner_id, created_at DESC)");
    execute("SELECT remove_retention_policy('raw_shares', if_exists => TRUE)");
    ensureRetentionPolicy("raw_shares", "6 hours");
    execute("SELECT remove_compression_policy('raw_shares', if_exists => TRUE)");

    // ---------- hashrates ----------
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS hashrates (
            miner_id INTEGER NOT NULL REFERENCES miners(id) ON DELETE CASCADE,
            hashrate DOUBLE PRECISION NOT NULL,
            created_at TIMESTAMPTZ NOT NULL
        )
    )SQL");
    ensureHypertable("hashrates", "created_at");
    ensureIndex("idx_hashrates_miner_ts",
                "CREATE INDEX idx_hashrates_miner_ts ON hashrates(miner_id, created_at DESC)");
    ensureRetentionPolicy("hashrates", "30 days");
    ensureCompression("hashrates", "created_at DESC");
    ensureCompressionPolicy("hashrates", "3 days");

    // ---------- blocks ----------
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS blocks (
            id SERIAL PRIMARY KEY,
            height BIGINT NOT NULL,
            block_hash TEXT NOT NULL UNIQUE,
            found_by INTEGER REFERENCES miners(id) ON DELETE SET NULL,
            difficulty DOUBLE PRECISION,
            reward_value BIGINT,
            found_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            round_effort DOUBLE PRECISION,
            net_difficulty DOUBLE PRECISION,
            finder_effort DOUBLE PRECISION,
            coin TEXT
        )
    )SQL");
    ensureIndex("idx_blocks_found_by", "CREATE INDEX idx_blocks_found_by ON blocks(found_by)");
    ensureIndex("idx_blocks_found_at", "CREATE INDEX idx_blocks_found_at ON blocks(found_at DESC)");

    // ---------- network_stats (one row per coin, upserted by NetworkStatsTracker) ----------
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS network_stats (
            coin TEXT PRIMARY KEY,
            network_type TEXT NOT NULL,
            network_hashrate DOUBLE PRECISION NOT NULL,
            network_difficulty DOUBLE PRECISION NOT NULL,
            block_time INTEGER NOT NULL,
            next_target TEXT NOT NULL,
            next_bits TEXT NOT NULL,
            last_block_time TIMESTAMPTZ NOT NULL,
            block_height BIGINT NOT NULL,
            block_reward DOUBLE PRECISION NOT NULL,
            updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
    )SQL");

    // ---------- pool_status ----------
    // One row per coin holding THIS pool process's boot time, so the stats API
    // can report the pool engine's own uptime (rather than the API service's).
    // Stamped on every start, so uptime correctly resets on restart/redeploy.
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS pool_status (
            coin       TEXT PRIMARY KEY,
            started_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
    )SQL");
    execute(
        "INSERT INTO pool_status (coin, started_at) VALUES ($1, now()) "
        "ON CONFLICT (coin) DO UPDATE SET started_at = now()",
        config.activeCoin);

    // ---------- network_difficulty_history ----------
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS network_difficulty_history (
            coin TEXT NOT NULL,
            difficulty DOUBLE PRECISION NOT NULL,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
    )SQL");
    ensureHypertable("network_difficulty_history", "created_at");
    ensureIndex("idx_network_diff_history_coin_ts",
                "CREATE INDEX idx_network_diff_history_coin_ts ON network_difficulty_history(coin, created_at DESC)");
    execute("SELECT remove_retention_policy('network_difficulty_history', if_exists => TRUE)");
    ensureRetentionPolicy("network_difficulty_history", "30 days");

    execute("ALTER TABLE network_difficulty_history SET (timescaledb.compress, timescaledb.compress_segmentby = 'coin')");
    execute("SELECT add_compression_policy('network_difficulty_history', INTERVAL '3 days', if_not_exists => TRUE)");

    // ---------- address_blacklist (exact-match, loaded into memory) ----------
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS address_blacklist (
            address    TEXT PRIMARY KEY,
            reason     TEXT,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
    )SQL");

    // ---------- workers_blacklist (normalized substring words) ----------
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS workers_blacklist (
            word       TEXT PRIMARY KEY,
            reason     TEXT,
            created_at TIMESTAMPTZ NOT NULL DEFAULT now()
        )
    )SQL");
    // Seed a baseline of clearly-offensive words ONLY on first creation, so
    // operator edits (removals/additions) persist across restarts.
    execute(R"SQL(
        INSERT INTO workers_blacklist (word, reason)
        SELECT v.word, 'default'
        FROM (VALUES
            ('fuck'),('shit'),('cunt'),('bitch'),('whore'),('slut'),('pussy'),
            ('nigger'),('nigga'),('faggot'),('retard'),('nazi'),('hitler'),
            ('pedo'),('kike'),('chink'),('gook'),('wetback'),('beaner'),
            ('tranny'),('asshole'),('dickhead'),('motherfuck'),('bullshit'),
            ('dumbass'),('jackass'),('bastard'),('wanker'),('bollocks'),
            ('douche'),('twat'),('penis'),('vagina'),('boobs'),('titties'),
            ('blowjob'),('handjob'),('cumshot'),('jizz'),('porn'),('skank'),
            ('bukkake'),('queef'),('felch'),('goatse'),('molest')
        ) AS v(word)
        WHERE NOT EXISTS (SELECT 1 FROM workers_blacklist)
    )SQL");

    // ---------- free_hashrate_grants (reserved "FREE-HASHRATE" allowlist) ----
    execute(R"SQL(
        CREATE TABLE IF NOT EXISTS free_hashrate_grants (
            address    TEXT PRIMARY KEY,
            granted_at TIMESTAMPTZ NOT NULL DEFAULT now(),
            expires_at TIMESTAMPTZ
        )
    )SQL");
    ensureIndex("idx_free_grants_expires",
                "CREATE INDEX idx_free_grants_expires ON free_hashrate_grants(expires_at)");
}

pqxx::connection& Database::conn() {
    // Callers hold conn_mutex_. Fast path: live connection.
    if (connection_ && connection_->is_open())
        return *connection_;

    // The server may have restarted out from under us (e.g. a needrestart
    // bounce after a libssl upgrade) and closed our socket. Rebuild it instead
    // of failing every query until the whole process is manually restarted.
    if (conn_str_.empty())
        throw std::runtime_error("Database not initialized");

    connection_ = std::make_unique<pqxx::connection>(conn_str_);  // throws if down
    if (!connection_->is_open()) {
        connection_.reset();
        throw std::runtime_error("Database connection failed");
    }
    spdlog::warn("[DB] reconnected");
    return *connection_;
}

void Database::shutdown() {
    std::lock_guard lk(conn_mutex_);
    connection_.reset();
    spdlog::info("[DB] connection closed");
}

void Database::execute(const std::string& sql) {
    std::lock_guard lk(conn_mutex_);
    for (int attempt = 0;; ++attempt) {
        try {
            pqxx::work txn(conn());
            txn.exec(sql);
            txn.commit();
            return;
        } catch (const pqxx::broken_connection& e) {
            connection_.reset();  // force a fresh connect on the next conn()
            if (attempt >= 1) {
                spdlog::critical("[DB] execute failed: {} - sql: {}", e.what(), sql);
                return;
            }
        } catch (const std::exception& e) {
            spdlog::critical("[DB] execute failed: {} - sql: {}", e.what(), sql);
            return;
        }
    }
}

void Database::ensureHypertable(const std::string& table, const std::string& timeColumn) {
    try {
        auto rows = query("SELECT 1 FROM timescaledb_information.hypertables "
                          "WHERE hypertable_name = $1", table);
        if (rows.empty())
            execute("SELECT create_hypertable($1, $2, if_not_exists => TRUE)", table, timeColumn);
    } catch (const std::exception& e) {
        spdlog::warn("[DB] ensureHypertable({}): {}", table, e.what());
    }
}
void Database::ensureIndex(const std::string& name, const std::string& sql) {
    try {
        auto rows = query("SELECT 1 FROM pg_indexes WHERE indexname = $1", name);
        if (rows.empty()) execute(sql);
    } catch (const std::exception& e) {
        spdlog::warn("[DB] ensureIndex({}): {}", name, e.what());
    }
}
void Database::ensureRetentionPolicy(const std::string& table, const std::string& interval) {
    try {
        auto rows = query("SELECT 1 FROM timescaledb_information.jobs "
                          "WHERE proc_name='policy_retention' AND hypertable_name=$1", table);
        if (rows.empty())
            execute("SELECT add_retention_policy($1, INTERVAL '" + interval + "')", table);
    } catch (const std::exception& e) {
        spdlog::warn("[DB] ensureRetentionPolicy({}): {}", table, e.what());
    }
}
void Database::ensureCompression(const std::string& table, const std::string& orderby) {
    try {
        auto rows = query("SELECT 1 FROM timescaledb_information.hypertables "
                          "WHERE hypertable_name=$1 AND compression_enabled=true", table);
        if (rows.empty())
            execute("ALTER TABLE " + table +
                    " SET (timescaledb.compress, timescaledb.compress_orderby = '" + orderby + "')");
    } catch (const std::exception& e) {
        spdlog::warn("[DB] ensureCompression({}): {}", table, e.what());
    }
}
void Database::ensureCompressionPolicy(const std::string& table, const std::string& interval) {
    try {
        auto rows = query("SELECT 1 FROM timescaledb_information.jobs "
                          "WHERE proc_name='policy_compression' AND hypertable_name=$1", table);
        if (rows.empty())
            execute("SELECT add_compression_policy($1, INTERVAL '" + interval + "')", table);
    } catch (const std::exception& e) {
        spdlog::warn("[DB] ensureCompressionPolicy({}): {}", table, e.what());
    }
}
void Database::ensureContinuousAggregatePolicy(const std::string& view,
                                               const std::string& startOffset,
                                               const std::string& endOffset,
                                               const std::string& scheduleInterval) {
    try {
        auto rows = query("SELECT 1 FROM timescaledb_information.continuous_aggregates "
                          "WHERE view_name = $1 LIMIT 1", view);
        if (rows.empty()) {
            execute(fmt::format(
                "SELECT add_continuous_aggregate_policy('{}', "
                "start_offset => INTERVAL '{}', "
                "end_offset => INTERVAL '{}', "
                "schedule_interval => INTERVAL '{}')",
                view, startOffset, endOffset, scheduleInterval));
        }
    } catch (const std::exception& e) {
        spdlog::warn("[DB] ensureContinuousAggregatePolicy({}): {}", view, e.what());
    }
}

namespace {
    inline std::string miner_cache_key(const std::string& address,
                                       const std::string& worker) {
        std::string k;
        k.reserve(address.size() + worker.size() + 1);
        k = address;
        k.push_back('\x1f'); // unit separator: cannot appear in addr/worker
        k += worker;
        return k;
    }
} // namespace

int Database::miner_cache_get(const std::string& address, const std::string& worker) {
    std::lock_guard lk(miner_cache_mu_);
    auto it = miner_cache_.find(miner_cache_key(address, worker));
    return it == miner_cache_.end() ? -1 : it->second;
}

void Database::miner_cache_put(const std::string& address, const std::string& worker, int id) {
    if (id <= 0) return;
    std::lock_guard lk(miner_cache_mu_);
    // Crude unbounded-growth guard: a single pool process is unlikely to ever
    // see this many distinct (address,worker) pairs, but never let the cache
    // grow without limit.
    if (miner_cache_.size() > 1'000'000) miner_cache_.clear();
    miner_cache_[miner_cache_key(address, worker)] = id;
}

int Database::ensure_miner(const std::string& address, const std::string& worker,
                           const std::string& ip) {
    // Fast path: already resolved. No DB round-trip, no conn_mutex_ - so the
    // common case (reconnects / repeat authorizes) never touches the io-thread-
    // serialising database lock.
    if (int id = miner_cache_get(address, worker); id > 0) return id;

    // Cold path: first time we see this (address, worker). Do the upsert once
    // and cache the result.
    try {
        auto rows = query(
            "WITH ins AS ("
            "  INSERT INTO miners (btc_address, worker_name, status, last_known_ip) "
            "  VALUES ($1, $2, 'online', $3) "
            "  ON CONFLICT (btc_address, worker_name) DO UPDATE "
            "  SET status='online', last_known_ip=$3, updated_at=now() "
            "  RETURNING id"
            ") SELECT id FROM ins",
            address, worker, ip);
        if (!rows.empty() && !rows[0].empty()) {
            int id = std::stoi(rows[0][0]);
            miner_cache_put(address, worker, id);
            return id;
        }
    } catch (const std::exception& e) {
        spdlog::error("[DB] ensure_miner({}/{}): {}", address, worker, e.what());
    }
    return -1;
}

} // namespace mkpool
