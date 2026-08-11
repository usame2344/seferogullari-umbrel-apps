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
// File:        database.hpp
// Description: Database connection and schema interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <memory>
#include <mutex>
#include <pqxx/pqxx>
#include <string>
#include <unordered_map>
#include <vector>

namespace mkpool {

struct Config;

class Database {
public:
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    static void init(const Config& config);
    static void shutdown();

    // For schema / migration / read queries (slow path / REST API).
    static void execute(const std::string& sql);
    template <typename... Params>
    static void execute(const std::string& sql, Params&&... params);

    template <typename... Params>
    static std::vector<std::vector<std::string>>
        query(const std::string& sql, Params&&... params);

    // Resolve the miner id for (address, worker), used on the hot authorize /
    // reconnect path. Returns from an in-process cache without touching the DB
    // (or the global connection mutex) whenever the pair has been seen before;
    // only the first time for a given (address, worker) does it perform a DB
    // upsert. This keeps reconnect storms (MRR/NiceHash proxies re-auth'ing the
    // same workers many times a minute) off the io threads and the DB lock.
    static int ensure_miner(const std::string& address,
                            const std::string& worker,
                            const std::string& ip);

    // (address, worker) -> miner id cache. miner_cache_put is also called by the
    // async presence writer (db_worker) so the row it upserts pre-warms the
    // cache for the io threads.
    static int  miner_cache_get(const std::string& address, const std::string& worker);
    static void miner_cache_put(const std::string& address, const std::string& worker, int id);

    // Schema helpers (idempotent).
    static void ensureHypertable(const std::string& table, const std::string& timeColumn);
    static void ensureIndex(const std::string& indexName, const std::string& sql);
    static void ensureRetentionPolicy(const std::string& table, const std::string& interval);
    static void ensureCompression(const std::string& table, const std::string& orderby);
    static void ensureCompressionPolicy(const std::string& table, const std::string& interval);
    static void ensureContinuousAggregatePolicy(const std::string& view,
                                                const std::string& startOffset,
                                                const std::string& endOffset,
                                                const std::string& scheduleInterval);

private:
    Database() = default;
    // Returns a live connection, lazily reconnecting if the server dropped us.
    // Caller must hold conn_mutex_.
    static pqxx::connection& conn();
    static inline std::unique_ptr<pqxx::connection> connection_{nullptr};
    static inline std::string conn_str_;  // retained so conn() can reconnect.
    static inline std::mutex conn_mutex_;

    // miner-id cache (see ensure_miner). Guarded by its own mutex; the critical
    // section is a map lookup/insert only - never DB I/O - so it does not
    // serialise the io threads the way conn_mutex_ did.
    static inline std::mutex miner_cache_mu_;
    static inline std::unordered_map<std::string, int> miner_cache_;
};

namespace detail {
    // Implemented in db_worker.cpp; called by Database::init to share the
    // connection string with the worker thread.
    void set_db_conn_string(std::string s);
}

template <typename... Params>
void Database::execute(const std::string& sql, Params&&... params) {
    std::lock_guard lk(conn_mutex_);
    for (int attempt = 0;; ++attempt) {
        try {
            pqxx::work txn(conn());
            txn.exec(sql, pqxx::params{std::forward<Params>(params)...});
            txn.commit();
            return;
        } catch (const pqxx::broken_connection&) {
            connection_.reset();          // rebuild on next conn()
            if (attempt >= 1) throw;      // one retry, then surface to caller
        }
    }
}

template <typename... Params>
std::vector<std::vector<std::string>>
Database::query(const std::string& sql, Params&&... params) {
    std::lock_guard lk(conn_mutex_);
    for (int attempt = 0;; ++attempt) {
        try {
            pqxx::work txn(conn());
            auto result = txn.exec(sql, pqxx::params{std::forward<Params>(params)...});
            std::vector<std::vector<std::string>> rows;
            rows.reserve(result.size());
            for (const auto& row : result) {
                std::vector<std::string> r;
                r.reserve(row.size());
                for (const auto& f : row) r.emplace_back(f.is_null() ? std::string{} : f.template as<std::string>());
                rows.push_back(std::move(r));
            }
            txn.commit();
            return rows;
        } catch (const pqxx::broken_connection&) {
            connection_.reset();          // rebuild on next conn()
            if (attempt >= 1) throw;      // one retry, then surface to caller
        }
    }
}

} // namespace mkpool
