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
// File:        pool_manager.hpp
// Description: Coin-runtime and session-map manager interface.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "bitcoin_client.hpp"
#include "client_session.hpp"
#include "cluster_block_relay.hpp"
#include "cluster_trunk.hpp"
#include "config.hpp"
#include "connector.hpp"
#include "control_server.hpp"
#include "generator.hpp"
#include "io_pool.hpp"
#include "network_stats.hpp"
#include "proxy_session.hpp"
#include "rate_limiter.hpp"
#include "redirector_session.hpp"
#include "relay_listener.hpp"
#include "stratifier.hpp"
#include "sv2_relay_session.hpp"
#include "tls_context.hpp"
#include "upstream_client.hpp"
#include <boost/asio/steady_timer.hpp>
#include <chrono>

namespace mkpool {

class PoolManager {
public:
    PoolManager(boost::asio::io_context& main_io,
                IoPool& workers,
                const GlobalConfig& global,
                const std::vector<CoinConfig>& coins,
                std::shared_ptr<RateLimiter> rl);

    void start();
    void stop();

    // Graceful drain for a low-downtime restart. Instead of dropping every
    // miner at once (what stop() does), stop accepting new connections and
    // migrate live miners over `window_seconds`: V1 sessions get a staggered
    // client.reconnect, V2 sessions keep working and are closed at the end.
    // `on_done` runs once the drain completes (used to stop the io pool so the
    // process exits). Idempotent; a second call is ignored.
    void beginGracefulShutdown(int window_seconds, std::function<void()> on_done);

    // handle one control-socket command line and return a JSON response
    // string. Thread-safe: reads use the sharded session lock + lock-free
    // per-session snapshots; mutations post to each session's strand.
    std::string handleControlCommand(const std::string& line);

    // Rebuild every coin's TLS context from the configured cert/key and install
    // it for new connections (SIGHUP handler). In-flight sessions keep the
    // context they handshook with. Safe to call from a signal handler context.
    void reloadTls();

private:
    struct CoinRuntime {
        CoinConfig                            cfg;
        StratifierPtr                         stratifier;
        std::shared_ptr<bitcoin::BitcoinClient> btc;
        std::shared_ptr<bitcoin::BitcoinClient> auxBtc;
        std::shared_ptr<Generator>            generator;
        // One Connector per stratum tier (port + starting difficulty).
        // tierCfgs holds per-tier CoinConfig copies that Connector/ClientSession
        // reference by const&; the vector is reserved before push_back so the
        // refs stay stable for the program lifetime.
        std::vector<std::unique_ptr<CoinConfig>> tierCfgs;
        std::vector<std::shared_ptr<Connector>> connectors;
        // Shared by all of this coin's TLS tiers; null when the coin has no TLS
        // tier (or the cert/key failed to load). Swappable for cert reloads.
        std::shared_ptr<TlsReloadable>          tls;
        std::shared_ptr<NetworkStatsTracker>    statsTracker;
        // Second tracker for the merged-mining aux chain (e.g. DOGE under LTC),
        // so the aux coin gets its own network_stats / difficulty_history rows.
        std::shared_ptr<NetworkStatsTracker>    auxStatsTracker;

        // Scale-out roles (all null in the default pool role). Populated only by
        // startRelayCoin() when global.role selects a relay role.
        std::shared_ptr<UpstreamSource>              upstream;       // proxy (V1 or SV2 upstream)
        RedirectorPolicyPtr                          redirectPolicy; // redirector
        std::vector<std::shared_ptr<RelayListener>>  relayListeners; // proxy/redirector/cluster-ingest
        std::vector<std::shared_ptr<Sv2RelayListener>> sv2Listeners; // proxy SV2->V1 downstream tiers
        ClusterTrunkPtr                              clusterTrunk;   // passthrough/node edge uplink
        ClusterBlockRelayPtr                         clusterBlockRelay; // origin node-block fan-out
    };

    struct SessionShard {
        mutable std::shared_mutex                                 mu;
        std::unordered_map<std::uint64_t, std::shared_ptr<ClientSession>> map;
    };

    void onNewJob(const std::string& coin_name, JobPtr job);
    void registerSession(std::shared_ptr<ClientSession> s);
    void unregisterSession(const std::shared_ptr<ClientSession>& s);

    // Bring up a coin runtime for a scale-out role (proxy / redirector). Never
    // called in the default pool role, so the solo wiring in start() is untouched.
    void startRelayCoin(CoinRuntime& rt);
    // Pool-role origin ingest: accept cluster trunks for this coin and bridge each
    // multiplexed downstream miner to its own local stratum port. Additive; only
    // called when the coin has cluster.ingestPort set.
    void startClusterIngest(CoinRuntime& rt);
    // Shared: derive the downstream listen tiers (port/tls) from a coin config.
    [[nodiscard]] std::vector<CoinConfig::StratumTier> relayTiers(const CoinConfig& cfg) const;

    // Build an OpenSSL context from global_.tls; returns null (and logs) if the
    // cert/key are unset or fail to load.
    [[nodiscard]] std::shared_ptr<boost::asio::ssl::context> buildTlsContext() const;

    boost::asio::io_context& main_io_;
    IoPool& workers_;
    const GlobalConfig& global_;
    std::shared_ptr<RateLimiter> rl_;

    std::vector<CoinRuntime> coins_;

    // Sharded session map keyed by session pointer hash.
    std::vector<std::unique_ptr<SessionShard>> shards_;
    std::atomic<std::uint64_t> next_session_id_{1};

    std::atomic<bool> running_{false};

    // runtime control socket + process start time (for uptime).
    std::unique_ptr<ControlServer> control_;
    std::chrono::steady_clock::time_point started_at_{};
    std::int64_t started_unix_{0};

    // optional dead-worker reap timer (enabled when idleDropSeconds > 0).
    std::unique_ptr<boost::asio::steady_timer> idle_timer_;
    void scheduleIdleSweep();

    // Graceful-drain state (restartDrainSeconds). Guards against re-entry, holds
    // the wave timer, and remembers the callback that stops the io pool once the
    // drain finishes.
    std::atomic<bool> draining_{false};
    std::unique_ptr<boost::asio::steady_timer> drain_timer_;
    std::function<void()> on_drained_;
    void scheduleDrainWave(
        std::shared_ptr<std::vector<std::shared_ptr<ClientSession>>> sessions,
        std::size_t sent, int window_seconds);
    void finishShutdown();

    // Look up a live session by its control-plane id (the shard-map key).
    std::shared_ptr<ClientSession> sessionById(std::uint64_t id);
};

} // namespace mkpool
