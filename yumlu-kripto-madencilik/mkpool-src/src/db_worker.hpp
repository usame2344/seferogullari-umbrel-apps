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
// File:        db_worker.hpp
// Description: Asynchronous database writer interface.
// Created:     2026-06-05
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mkpool {

    struct ShareRecord {
        int    miner_id{-1};
        std::string job_id;
        std::string nonce;
        std::string extranonce1;
        std::string extranonce2;
        std::string share_hash;
        double difficulty{0.0};
        bool   accepted{false};
        bool   block_found{false};
    };

    struct PresenceRecord {
        std::string btc_address;
        std::string worker_name;
        std::string status; // "online" / "offline" / "banned"
        std::string ip;
    };

    struct BestShareRecord {
        int miner_id;
        double difficulty;
        std::string hash;
    };

    struct HashrateRecord {
        int miner_id;
        double hashrate_hps;
        std::chrono::system_clock::time_point ts;
    };

    // Found-block persistence (separate from raw_shares.block_found flag so
    // operators can query a clean history of solved blocks).
    struct BlockRecord {
        std::int64_t height{0};
        std::string  block_hash;
        int          miner_id{-1};
        double       difficulty{0.0};
        std::int64_t reward_value{0};
        // Coin ticker for this block. Empty => the process's primary coin (set
        // from DbWorkerConfig::coin). Set explicitly (e.g. "DOGE") for aux
        // merge-mined blocks so they are stored without disturbing the parent
        // coin's effort accounting. See db_worker.cpp.
        std::string  coin{};
    };

    struct DbWorkerConfig {
        std::size_t batch_size{256};
        std::chrono::milliseconds flush_interval{std::chrono::milliseconds(500)};
        std::size_t max_queue{1u << 16};
        double share_multiplier{4294967296.0};
        // This pool process's primary coin ticker (e.g. "LTC", "BTC"). Used as
        // the default coin for blocks and to tell parent blocks (which drive the
        // effort accumulator) apart from aux blocks (e.g. DOGE under LTC).
        std::string coin{"BTC"};
    };

    class DbWorker {
    public:
        using Config = DbWorkerConfig;

        explicit DbWorker(Config cfg = Config{});
        ~DbWorker();

        void start();
        void stop();

        // Enqueue helpers. Returns false if the queue is saturated (caller
        // should treat as a back-pressure event and either drop or log).
        bool enqueue_share(ShareRecord r);
        bool enqueue_presence(PresenceRecord r);
        bool enqueue_best_share(BestShareRecord r);
        bool enqueue_hashrate(HashrateRecord r);
        bool enqueue_block(BlockRecord r);

        struct Stats {
            std::uint64_t shares_written{0};
            std::uint64_t shares_dropped{0};
            std::uint64_t batches{0};
            std::size_t  queue_depth{0};
        };
        [[nodiscard]] Stats stats() const;

    private:
        void run();
        void flush_shares(std::vector<ShareRecord>& batch);

        Config cfg_;
        std::mutex mu_;
        std::condition_variable cv_;
        std::deque<ShareRecord>    shares_;
        std::deque<PresenceRecord> presences_;
        std::deque<BestShareRecord> best_shares_;
        std::deque<HashrateRecord> hashrates_;
        std::deque<BlockRecord>    blocks_;

        std::atomic<bool> running_{false};
        std::jthread thread_;

        // stats
        std::atomic<std::uint64_t> shares_written_{0};
        std::atomic<std::uint64_t> shares_dropped_{0};
        std::atomic<std::uint64_t> batches_{0};
    };

    // Process-wide singleton accessor (created in mkpool main).
    DbWorker& db_worker();
    void      set_db_worker(std::unique_ptr<DbWorker> w);

} // namespace mkpool
