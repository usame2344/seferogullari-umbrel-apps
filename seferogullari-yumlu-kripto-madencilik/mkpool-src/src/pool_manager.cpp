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
// File:        pool_manager.cpp
// Description: Owns per-coin runtimes and the sharded session map; broadcasts new jobs.
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "pool_manager.hpp"
#include "cluster_ingest.hpp"
#include "cluster_block_relay.hpp"
#include "sv2_upstream_client.hpp"
#include "metrics.hpp"
#include "blacklist.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace mkpool {

std::shared_ptr<boost::asio::ssl::context> PoolManager::buildTlsContext() const {
    if (global_.tls.certFile.empty() || global_.tls.keyFile.empty()) {
        spdlog::error("[PoolManager] TLS requested but global.tls certFile/keyFile "
                      "are not configured");
        return nullptr;
    }
    TlsConfig tc;
    tc.cert_file     = global_.tls.certFile;
    tc.key_file      = global_.tls.keyFile;
    tc.dhparams_file = global_.tls.dhparamsFile;
    try {
        return make_tls_context(tc);
    } catch (const std::exception& e) {
        spdlog::error("[PoolManager] failed to build TLS context (cert='{}' key='{}'): {}",
                      global_.tls.certFile, global_.tls.keyFile, e.what());
        return nullptr;
    }
}

void PoolManager::reloadTls() {
    for (auto& rt : coins_) {
        if (!rt.tls) continue; // coin has no TLS tier
        if (auto ctx = buildTlsContext()) {
            rt.tls->set(std::move(ctx));
            spdlog::info("[PoolManager] {} TLS certificate reloaded", rt.cfg.name);
        } else {
            // Keep serving the previously-loaded context rather than dropping TLS.
            spdlog::error("[PoolManager] {} TLS reload failed; keeping current "
                          "certificate", rt.cfg.name);
        }
    }
}

PoolManager::PoolManager(boost::asio::io_context& main_io,
                         IoPool& workers,
                         const GlobalConfig& global,
                         const std::vector<CoinConfig>& coins,
                         std::shared_ptr<RateLimiter> rl)
    : main_io_(main_io), workers_(workers), global_(global), rl_(std::move(rl)) {
    shards_.reserve(global_.sessionShards);
    for (std::uint32_t i = 0; i < global_.sessionShards; ++i) {
        shards_.emplace_back(std::make_unique<SessionShard>());
    }
    coins_.reserve(coins.size());
    for (const auto& c : coins) {
        CoinRuntime rt;
        rt.cfg = c;
        coins_.push_back(std::move(rt));
    }
}

void PoolManager::start() {
    running_.store(true);
    started_at_ = std::chrono::steady_clock::now();
    started_unix_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    spdlog::info("[PoolManager] operating role: {}", role_to_string(global_.role));
    for (auto& rt : coins_) {
        // Scale-out roles use a completely separate wiring path (no generator,
        // stratifier or bitcoind); the solo body below runs only for role=pool.
        if (global_.role != PoolRole::Pool) { startRelayCoin(rt); continue; }

        bool isNoSegwit = (rt.cfg.chain == ChainKind::BitcoinCash ||
                           rt.cfg.chain == ChainKind::BitcoinCashII ||
                           rt.cfg.chain == ChainKind::eCash ||
                           rt.cfg.chain == ChainKind::Zcash ||
                           rt.cfg.chain == ChainKind::Dogecoin);
        rt.btc = std::make_shared<bitcoin::BitcoinClient>(
            main_io_, rt.cfg.rpcHost, rt.cfg.rpcPort,
            rt.cfg.rpcUser, rt.cfg.rpcPassword,
            isNoSegwit, rt.cfg.chain);

        // Register optional failover nodes, then start the primary-recovery
        // watchdog (a no-op unless at least one fallback exists).
        for (const auto& fb : rt.cfg.rpcFallbacks) {
            rt.btc->addFallbackNode(fb.host, fb.port, fb.user, fb.pass);
        }
        // Register optional redundant-submit endpoints (host:port), reusing
        // the coin's primary RPC creds. Fire-and-forget on every submitblock.
        for (const auto& ep : rt.cfg.additionalSubmitEndpoints) {
            const auto colon = ep.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= ep.size()) {
                spdlog::warn("[PoolManager] {} ignoring malformed additionalSubmitEndpoint '{}'",
                             rt.cfg.name, ep);
                continue;
            }
            rt.btc->addSubmitEndpoint(ep.substr(0, colon), ep.substr(colon + 1),
                                      rt.cfg.rpcUser, rt.cfg.rpcPassword);
        }
        rt.btc->startWatchdog();

        if (rt.cfg.aux.enabled) {
            rt.auxBtc = std::make_shared<bitcoin::BitcoinClient>(
                main_io_, rt.cfg.aux.rpcHost, rt.cfg.aux.rpcPort,
                rt.cfg.aux.rpcUser, rt.cfg.aux.rpcPassword,
                true, ChainKind::Dogecoin);
        }

        rt.stratifier = std::make_shared<Stratifier>(rt.cfg.jobWindowSize);
        rt.stratifier->setCoinConfig(rt.cfg);

        const std::string coin_name = rt.cfg.name;
        auto strat_weak = std::weak_ptr<Stratifier>(rt.stratifier);
        rt.stratifier->registerCallback(
            [this, coin_name](JobPtr j) { this->onNewJob(coin_name, j); });

        rt.generator = std::make_shared<Generator>(
            main_io_, rt.btc, rt.cfg.useZMQ,
            std::chrono::seconds(rt.cfg.blockPollInterval > 0 ? rt.cfg.blockPollInterval : 10));
        rt.generator->setZmqEndpoints(rt.cfg.zmq.hashblock, rt.cfg.zmq.rawblock, rt.cfg.zmq.rawtx);
        
        if (rt.cfg.aux.enabled && rt.auxBtc) {
            rt.generator->setAuxClient(rt.auxBtc, rt.cfg.aux.payoutAddress);
        }

        auto strat = rt.stratifier;
        rt.generator->setBlockTemplateCallback([strat](const nlohmann::json& gbt) {
            if (auto s = strat) s->updateWork(gbt);
        });
        rt.generator->start();

        // Stratum tiers are the sole source of truth for listener ports and
        // their difficulty/vardiff/NH-gate config. If `stratumTiers` is empty
        // we fall back to a single tier on the legacy `stratumListenPort`
        // with a sensible default diff so a barebones config still boots.
        std::vector<CoinConfig::StratumTier> tiers = rt.cfg.stratumTiers;
        if (tiers.empty()) {
            CoinConfig::StratumTier t;
            t.port               = rt.cfg.stratumListenPort;
            t.label              = "default";
            t.startingDifficulty = 65536.0;
            tiers.push_back(t);
        }

        // If any tier requests TLS, build the shared per-coin OpenSSL context
        // once (from global cert/key). TLS tiers are skipped entirely if it
        // fails to load, so we never expose plaintext on a port advertised as
        // encrypted.
        const bool anyTls =
            std::any_of(tiers.begin(), tiers.end(),
                        [](const CoinConfig::StratumTier& t) { return t.tls; });
        if (anyTls) {
            if (auto ctx = buildTlsContext()) {
                rt.tls = std::make_shared<TlsReloadable>();
                rt.tls->set(std::move(ctx));
                spdlog::info("[PoolManager] {} TLS context loaded ({})",
                             rt.cfg.name, global_.tls.certFile);
            } else {
                spdlog::error("[PoolManager] {} has TLS tier(s) but no usable "
                              "cert/key; those ports will NOT be opened",
                              rt.cfg.name);
            }
        }

        rt.connectors.reserve(tiers.size());
        rt.tierCfgs.reserve(tiers.size());
        for (const auto& t : tiers) {
            if (t.port == 0) continue;
            if (t.tls && !rt.tls) {
                spdlog::warn("[PoolManager] {} skipping TLS tier {} :{} (no context)",
                             rt.cfg.name, t.label.empty() ? "unnamed" : t.label, t.port);
                continue;
            }
            auto tier_cfg = std::make_unique<CoinConfig>(rt.cfg);
            tier_cfg->stratumListenPort = t.port;
            tier_cfg->activeTier        = t;
            spdlog::info("[PoolManager] {} tier {} :{} startDiff={:.0f}{}{}{}",
                         rt.cfg.name,
                         t.label.empty() ? "unnamed" : t.label,
                         t.port, t.startingDifficulty,
                         t.vardiffEnabled
                             ? fmt::format(" vardiff=[{:.0f},{:.0f}]", t.vardiffMin, t.vardiffMax)
                             : std::string{" fixed"},
                         t.allowNiceHash ? " nh=allowed" : "",
                         t.tls ? " tls" : "");
            const CoinConfig& cfg_ref = *tier_cfg;
            rt.tierCfgs.push_back(std::move(tier_cfg));
            auto conn = std::make_shared<Connector>(
                main_io_, workers_, cfg_ref, rt.stratifier, rt.btc, rl_,
                [this](std::shared_ptr<ClientSession> s) {
                    registerSession(s);
                    s->setDisconnectHandler(
                        [this](std::shared_ptr<ClientSession> dead) { unregisterSession(dead); });
                }, rt.auxBtc,
                t.tls ? rt.tls : nullptr);
            conn->start();
            rt.connectors.push_back(std::move(conn));
        }

        if (rt.cfg.stratumV2Port != 0) {
            auto tier_cfg = std::make_unique<CoinConfig>(rt.cfg);
            tier_cfg->stratumListenPort = rt.cfg.stratumV2Port;
            CoinConfig::StratumTier t;
            t.port               = rt.cfg.stratumV2Port;
            t.label              = "stratumv2";
            t.sv2                = true;   // exclusive-V2 detection now keys off this flag
            t.startingDifficulty = rt.cfg.stratumV2Difficulty > 0 ? rt.cfg.stratumV2Difficulty : 1024.0;
            // v2 channel runs the same vardiff controller as V1 (retargets via SetTarget).
            t.vardiffEnabled     = true;
            t.vardiffMin         = 1024.0;
            t.vardiffMax         = 1000000.0;
            tier_cfg->activeTier        = t;
            
            spdlog::info("[PoolManager] {} StratumV2 connector :{}", rt.cfg.name, rt.cfg.stratumV2Port);
            
            const CoinConfig& cfg_ref = *tier_cfg;
            rt.tierCfgs.push_back(std::move(tier_cfg));
            
            auto conn = std::make_shared<Connector>(
                main_io_, workers_, cfg_ref, rt.stratifier, rt.btc, rl_,
                [this](std::shared_ptr<ClientSession> s) {
                    registerSession(s);
                    s->setDisconnectHandler(
                        [this](std::shared_ptr<ClientSession> dead) { unregisterSession(dead); });
                }, rt.auxBtc);
            conn->start();
            rt.connectors.push_back(std::move(conn));
        }

        // Network stats tracker: pulls chain info on every ZMQ hashblock
        // notification + a 30s heartbeat, and upserts a per-coin row into
        // `network_stats` for the REST API to read.
        rt.statsTracker = std::make_shared<NetworkStatsTracker>(
            main_io_, rt.btc, rt.cfg, std::chrono::seconds(30));
        // The Generator already owns the single hashblock ZMQ subscription for
        // this coin. Drive stats refreshes from it (via the block-notify
        // callback) instead of opening a second, duplicate subscription to the
        // same node. Must be set before start() so it skips its own ZMQ.
        rt.statsTracker->setExternalBlockFeed(true);
        rt.statsTracker->start();
        if (rt.generator) {
            std::weak_ptr<NetworkStatsTracker> ws = rt.statsTracker;
            rt.generator->setBlockNotifyCallback([ws] {
                if (auto s = ws.lock()) s->notifyNewBlock();
            });
        }

        // Merged mining: the aux chain (DOGE) has its own chain stats that the
        // REST API serves on the DOGE pool page. The primary tracker above only
        // writes the parent coin (LTC), so spin up a second tracker bound to the
        // aux RPC client. Heartbeat-only at 30s since the aux config carries no
        // ZMQ endpoint; subsidy_for_height() already knows DOGE.
        if (rt.cfg.aux.enabled && rt.auxBtc) {
            CoinConfig auxCoin;
            auxCoin.name  = "DOGE";
            auxCoin.chain = ChainKind::Dogecoin;
            auxCoin.zmqAddress.clear(); // avoid subscribing to the parent's ZMQ
            auxCoin.zmq = {};
            rt.auxStatsTracker = std::make_shared<NetworkStatsTracker>(
                main_io_, rt.auxBtc, auxCoin, std::chrono::seconds(30));
            rt.auxStatsTracker->start();
            spdlog::info("[PoolManager] {} aux NetStats tracker started (DOGE)", rt.cfg.name);
        }

        // Origin-side cluster ingest (opt-in; default off). Accepts multiplexed
        // passthrough/node trunks for this coin and bridges each downstream miner
        // to this coin's own local stratum port, so clustered miners are served
        // exactly like direct ones with no change to the solo path above.
        if (rt.cfg.cluster.ingest_on()) startClusterIngest(rt);
    }

    // Start the runtime control socket. Path is resolved by mkpool.cpp;
    // "off"/"none"/"-" disables it. A bind failure never stops the pool.
    const std::string& csock = global_.controlSocket;
    if (!csock.empty() && csock != "off" && csock != "none" && csock != "-") {
        control_ = std::make_unique<ControlServer>(
            main_io_, csock,
            [this](const std::string& line) { return handleControlCommand(line); });
        control_->start();
    }

    // Opt-in dead-worker reap sweep.
    if (global_.idleDropSeconds > 0) {
        idle_timer_ = std::make_unique<boost::asio::steady_timer>(main_io_);
        scheduleIdleSweep();
        spdlog::info("[PoolManager] idle-drop sweep enabled ({}s)", global_.idleDropSeconds);
    }
}

std::vector<CoinConfig::StratumTier> PoolManager::relayTiers(const CoinConfig& cfg) const {
    std::vector<CoinConfig::StratumTier> tiers = cfg.stratumTiers;
    if (tiers.empty()) {
        CoinConfig::StratumTier t;
        t.port  = cfg.stratumListenPort;
        t.label = "default";
        tiers.push_back(t);
    }
    return tiers;
}

void PoolManager::startRelayCoin(CoinRuntime& rt) {
    const PoolRole role = global_.role;

    auto tiers = relayTiers(rt.cfg);

    // Build the shared TLS context once if any downstream tier wants TLS.
    const bool anyTls = std::any_of(tiers.begin(), tiers.end(),
                                    [](const CoinConfig::StratumTier& t) { return t.tls; });
    if (anyTls) {
        if (auto ctx = buildTlsContext()) {
            rt.tls = std::make_shared<TlsReloadable>();
            rt.tls->set(std::move(ctx));
        } else {
            spdlog::error("[PoolManager] {} has TLS relay tier(s) but no usable cert/key; "
                          "those ports will NOT be opened", rt.cfg.name);
        }
    }

    // Role-specific backend setup + downstream session factory.
    RelayListener::Factory factory;
    std::string label;

    if (role == PoolRole::Proxy) {
        if (rt.cfg.chain == ChainKind::Zcash) {
            spdlog::critical("[PoolManager] {} proxy role does not support Equihash (ZEC) yet; skipping",
                             rt.cfg.name);
            return;
        }
        if (!rt.cfg.upstream.enabled()) {
            spdlog::critical("[PoolManager] {} proxy role requires 'upstream.endpoints'; skipping",
                             rt.cfg.name);
            return;
        }
        // upstream.sv2 selects an SV2 upstream (V1 miners -> SV2 pool); otherwise
        // the classic V1 upstream. Both implement UpstreamSource, so the downstream
        // session code is identical.
        if (rt.cfg.upstream.sv2)
            rt.upstream = std::make_shared<Sv2UpstreamClient>(main_io_, rt.cfg);
        else
            rt.upstream = std::make_shared<UpstreamClient>(main_io_, rt.cfg);
        rt.upstream->start();
        auto up = rt.upstream;
        auto* cfg = &rt.cfg;
        factory = [this, cfg, up](boost::asio::io_context& worker,
                                  std::shared_ptr<boost::asio::ssl::context> ctx)
                      -> std::shared_ptr<RelaySession> {
            return std::make_shared<ProxyDownstreamSession>(worker, *cfg, rl_, up, std::move(ctx));
        };
        label = rt.cfg.name + "-proxy";
    } else if (role == PoolRole::Redirector) {
        if (!rt.cfg.redirect.enabled()) {
            spdlog::critical("[PoolManager] {} redirector role requires 'redirect.targets'; skipping",
                             rt.cfg.name);
            return;
        }
        rt.redirectPolicy = std::make_shared<RedirectorPolicy>(main_io_, rt.cfg.redirect);
        rt.redirectPolicy->start();   // begins health probing if enabled
        auto pol = rt.redirectPolicy;
        factory = [this, pol](boost::asio::io_context& worker,
                              std::shared_ptr<boost::asio::ssl::context> ctx)
                      -> std::shared_ptr<RelaySession> {
            return std::make_shared<RedirectorSession>(worker, rl_, pol, std::move(ctx));
        };
        label = rt.cfg.name + "-redirector";
    } else { // Passthrough || Node - mkpool-native cluster edge
        if (rt.cfg.chain == ChainKind::Zcash) {
            spdlog::critical("[PoolManager] {} {} role does not support Equihash (ZEC) yet; skipping",
                             rt.cfg.name, role_to_string(role));
            return;
        }
        if (!rt.cfg.cluster.uplink_on()) {
            spdlog::critical("[PoolManager] {} {} role requires 'cluster.originHost'/'cluster.originPort'; skipping",
                             rt.cfg.name, role_to_string(role));
            return;
        }
        rt.clusterTrunk = std::make_shared<ClusterTrunk>(main_io_, rt.cfg, role == PoolRole::Node);
        rt.clusterTrunk->start();
        auto trunk = rt.clusterTrunk;
        factory = [this, trunk](boost::asio::io_context& worker,
                                std::shared_ptr<boost::asio::ssl::context> ctx)
                      -> std::shared_ptr<RelaySession> {
            return std::make_shared<ClusterEdgeSession>(worker, rl_, trunk, std::move(ctx));
        };
        label = rt.cfg.name + "-" + role_to_string(role);
    }

    // Open a downstream listener per tier.
    for (const auto& t : tiers) {
        if (t.port == 0) continue;
        // Stratum V2 (Noise) downstream tier: only the proxy role can translate an
        // SV2 miner onto its V1 upstream. Uses a dedicated Sv2RelayListener since
        // SV2 is a binary Noise protocol, not the line-based relay transport.
        if (t.sv2) {
            if (role != PoolRole::Proxy || !rt.upstream) {
                spdlog::warn("[PoolManager] {} SV2 downstream tier :{} is only supported in the proxy role; skipping",
                             rt.cfg.name, t.port);
                continue;
            }
            auto l = std::make_shared<Sv2RelayListener>(
                main_io_, workers_, rt.cfg.stratumListenAddress, t.port, rl_, rt.cfg,
                rt.upstream, rt.cfg.sv2AuthorityKey, rt.cfg.name + "-proxy-sv2");
            l->start();
            rt.sv2Listeners.push_back(std::move(l));
            continue;
        }
        if (t.tls && !rt.tls) {
            spdlog::warn("[PoolManager] {} skipping TLS relay tier :{} (no context)", rt.cfg.name, t.port);
            continue;
        }
        auto listener = std::make_shared<RelayListener>(
            main_io_, workers_, rt.cfg.stratumListenAddress, t.port, rl_, factory,
            t.tls ? rt.tls : nullptr, label);
        listener->start();
        rt.relayListeners.push_back(std::move(listener));
        spdlog::info("[PoolManager] {} {} listener on :{}{}",
                     rt.cfg.name, role_to_string(role), t.port, t.tls ? " (tls)" : "");
    }
}

// Origin-side (pool role) cluster ingest listener for one coin. Bridges each
// multiplexed downstream miner to this coin's own local stratum port. Only
// called when rt.cfg.cluster.ingest_on(); inert in a normal pool deployment.
void PoolManager::startClusterIngest(CoinRuntime& rt) {
    const auto& cl = rt.cfg.cluster;

    std::uint16_t fwd = cl.forwardPort;
    if (fwd == 0) {
        fwd = rt.cfg.stratumTiers.empty() ? rt.cfg.stratumListenPort
                                          : rt.cfg.stratumTiers.front().port;
    }
    if (fwd == 0) {
        spdlog::error("[PoolManager] {} cluster ingest: no forward port resolvable; skipping",
                      rt.cfg.name);
        return;
    }

    // TLS on the trunk acceptor (independent of the coin's miner-facing tiers).
    std::shared_ptr<TlsReloadable> ingestTls;
    if (cl.ingestTls) {
        if (auto ctx = buildTlsContext()) {
            ingestTls = std::make_shared<TlsReloadable>();
            ingestTls->set(std::move(ctx));
        } else {
            spdlog::error("[PoolManager] {} cluster ingestTls set but no usable cert/key; "
                          "ingest port NOT opened", rt.cfg.name);
            return;
        }
    }

    // Optional node block fan-out: subscribe to the origin node's rawblock ZMQ
    // and stream accepted blocks to node trunks for local submission.
    if (!cl.rawblockZmq.empty()) {
        rt.clusterBlockRelay = std::make_shared<ClusterBlockRelay>(main_io_, cl.rawblockZmq, rt.cfg.name);
        rt.clusterBlockRelay->start();
    }

    const std::string coin    = rt.cfg.name;
    const std::string token   = cl.token;
    const std::string fwdHost = cl.forwardHost;
    auto relay = rt.clusterBlockRelay;
    auto factory = [this, fwdHost, fwd, coin, token, relay](
                       boost::asio::io_context& worker,
                       std::shared_ptr<boost::asio::ssl::context> ctx)
                       -> std::shared_ptr<RelaySession> {
        return std::make_shared<ClusterIngestSession>(worker, rl_, std::move(ctx),
                                                      fwdHost, fwd, coin, token, relay);
    };
    auto listener = std::make_shared<RelayListener>(
        main_io_, workers_, cl.ingestAddress, cl.ingestPort, rl_, factory,
        ingestTls, rt.cfg.name + "-cluster-ingest");
    listener->start();
    rt.relayListeners.push_back(std::move(listener));
    spdlog::info("[PoolManager] {} cluster ingest on {}:{}{} -> {}:{}{}",
                 rt.cfg.name, cl.ingestAddress, cl.ingestPort,
                 cl.ingestTls ? " (tls)" : "", fwdHost, fwd,
                 rt.clusterBlockRelay ? " (+node block relay)" : "");
}

void PoolManager::stop() {
    running_.store(false);
    if (control_) control_->stop();
    if (idle_timer_) idle_timer_->cancel();
    for (auto& rt : coins_) {
        if (rt.statsTracker) rt.statsTracker->stop();
        if (rt.auxStatsTracker) rt.auxStatsTracker->stop();
        for (auto& c : rt.connectors) if (c) c->stop();
        if (rt.generator)  rt.generator->stop();
        // Scale-out roles: stop accepting and drop the outbound uplink/trunk.
        for (auto& l : rt.relayListeners) if (l) l->stop();
        for (auto& l : rt.sv2Listeners) if (l) l->stop();
        if (rt.upstream) rt.upstream->stop();
        if (rt.redirectPolicy) rt.redirectPolicy->stop();
        if (rt.clusterTrunk) rt.clusterTrunk->stop();
        if (rt.clusterBlockRelay) rt.clusterBlockRelay->stop();
    }
    // Close all sessions.
    for (auto& sh : shards_) {
        std::vector<std::shared_ptr<ClientSession>> snap;
        {
            std::shared_lock lk(sh->mu);
            snap.reserve(sh->map.size());
            for (auto& [_, s] : sh->map) snap.push_back(s);
        }
        for (auto& s : snap) s->shutdown();
    }
}

// Begin a graceful drain: stop accepting, then migrate live miners in staggered
// waves so a restart does not disconnect everyone at once. Runs on main_io_.
void PoolManager::beginGracefulShutdown(int window_seconds, std::function<void()> on_done) {
    if (draining_.exchange(true)) return;   // already draining
    on_drained_ = std::move(on_done);
    if (window_seconds < 1) window_seconds = 1;

    // Stop accepting new connections. A freshly-started instance that bound the
    // same ports via SO_REUSEPORT now serves all new connections; miners still
    // on this instance keep getting valid work from the still-running
    // generator/stratifier until they migrate.
    for (auto& rt : coins_) {
        for (auto& c : rt.connectors)
            if (c) c->stop();
        // Relay roles have no shard-tracked sessions to migrate; stop accepting
        // and drop the uplink so the drain window simply ends the process.
        for (auto& l : rt.relayListeners) if (l) l->stop();
        if (rt.upstream) rt.upstream->stop();
    }

    // Snapshot live sessions; the shared_ptr copies keep them alive across the
    // drain even as miners disconnect and their shard entries are erased.
    auto sessions = std::make_shared<std::vector<std::shared_ptr<ClientSession>>>();
    for (auto& sh : shards_) {
        std::shared_lock lk(sh->mu);
        sessions->reserve(sessions->size() + sh->map.size());
        for (auto& [id, s] : sh->map) sessions->push_back(s);
    }
    spdlog::warn("[PoolManager] graceful shutdown: migrating {} session(s) over {}s "
                 "(V1 via client.reconnect; V2 closed at end)",
                 sessions->size(), window_seconds);

    drain_timer_ = std::make_unique<boost::asio::steady_timer>(main_io_);
    scheduleDrainWave(std::move(sessions), 0, window_seconds);
}

// Send one wave's worth of client.reconnect, then re-arm for the next wave. Once
// every wave is sent, wait a short grace for miners to leave, force-close any
// stragglers (all V2 sessions, plus any V1 miner that ignored the reconnect),
// and finish the shutdown.
void PoolManager::scheduleDrainWave(
        std::shared_ptr<std::vector<std::shared_ptr<ClientSession>>> sessions,
        std::size_t sent, int window_seconds) {
    constexpr std::size_t kWaves = 10;   // spread reconnects into ~10 waves
    const std::size_t total = sessions->size();

    if (sent < total) {
        const std::size_t per_wave =
            std::max<std::size_t>(1, (total + kWaves - 1) / kWaves);
        const std::size_t end = std::min(total, sent + per_wave);
        // requestReconnect() posts to each session's strand and is a no-op for
        // already-closed sessions and for V2 (no such notification exists).
        for (std::size_t i = sent; i < end; ++i)
            if ((*sessions)[i]) (*sessions)[i]->requestReconnect({}, {}, 0);

        if (end < total) {
            const int gap_ms = std::max(1, (window_seconds * 1000) /
                                            static_cast<int>(kWaves));
            drain_timer_->expires_after(std::chrono::milliseconds(gap_ms));
            drain_timer_->async_wait(
                [this, sessions, end, window_seconds](const boost::system::error_code& ec) {
                    if (ec) return;
                    scheduleDrainWave(sessions, end, window_seconds);
                });
            return;
        }
    }

    const int grace_s = std::max(2, window_seconds / 5);
    drain_timer_->expires_after(std::chrono::seconds(grace_s));
    drain_timer_->async_wait(
        [this, sessions](const boost::system::error_code& ec) {
            if (ec) return;
            std::size_t forced = 0;
            for (auto& s : *sessions) if (s) { s->shutdown(); ++forced; }
            spdlog::warn("[PoolManager] drain window elapsed; force-closed {} remaining "
                         "session(s); stopping", forced);
            finishShutdown();
        });
}

void PoolManager::finishShutdown() {
    stop();   // idempotent: stops control/timers/stats/connectors/generator + sessions
    if (on_drained_) { auto cb = std::move(on_drained_); on_drained_ = nullptr; cb(); }
}

void PoolManager::onNewJob(const std::string& coin_name, JobPtr job) {
    if (!job) return;
    std::size_t broadcast = 0;
    for (auto& sh : shards_) {
        std::shared_lock lk(sh->mu);
        for (auto& [_, s] : sh->map) {
            if (s->coin().name == coin_name) {
                s->notifyNewJob(job);
                ++broadcast;
            }
        }
    }
    spdlog::debug("[PoolManager] {} broadcast job {} to {} sessions",
                  coin_name, job->job_id, broadcast);
    metrics::set_active_miners(static_cast<double>(broadcast));
}

void PoolManager::registerSession(std::shared_ptr<ClientSession> s) {
    auto id = next_session_id_.fetch_add(1, std::memory_order_relaxed);
    auto& sh = shards_[id % shards_.size()];
    std::unique_lock lk(sh->mu);
    sh->map.emplace(id, std::move(s));
}

void PoolManager::unregisterSession(const std::shared_ptr<ClientSession>& s) {
    if (!s) return;
    // Linear scan (rare path). We don't store the id back on the session so
    // this is O(shard_size). Acceptable since disconnects are infrequent.
    for (auto& sh : shards_) {
        std::unique_lock lk(sh->mu);
        for (auto it = sh->map.begin(); it != sh->map.end(); ++it) {
            if (it->second.get() == s.get()) {
                sh->map.erase(it);
                return;
            }
        }
    }
}

// Periodically reap connections that have gone silent longer than
// idleDropSeconds (measured from last accepted share, or connect time if the
// session never submitted). Runs on main_io_; only active when opted in.
void PoolManager::scheduleIdleSweep() {
    if (!idle_timer_) return;
    idle_timer_->expires_after(std::chrono::seconds(30));
    idle_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec || !running_.load()) return;
        const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::int64_t limit = static_cast<std::int64_t>(global_.idleDropSeconds);
        std::vector<std::shared_ptr<ClientSession>> victims;
        for (auto& sh : shards_) {
            std::shared_lock lk(sh->mu);
            for (auto& [id, s] : sh->map) {
                auto snap = s->statSnapshot();
                if (!snap) continue;
                const std::int64_t last = s->liveLastShareUnix();
                const std::int64_t base = last > 0 ? last : snap->connected_unix;
                if (base > 0 && (now - base) > limit) victims.push_back(s);
            }
        }
        for (auto& v : victims) v->shutdown();
        if (!victims.empty())
            spdlog::warn("[PoolManager] idle-drop reaped {} dead worker(s) (>{}s idle)",
                         victims.size(), limit);
        scheduleIdleSweep();
    });
}

std::shared_ptr<ClientSession> PoolManager::sessionById(std::uint64_t id) {
    auto& sh = shards_[id % shards_.size()];  // ids are placed in shard id%N (registerSession)
    std::shared_lock lk(sh->mu);
    auto it = sh->map.find(id);
    return it == sh->map.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Runtime control command dispatch. Returns a JSON response string. Reads
// are lock-free per-session snapshots gathered under the shard read-lock;
// mutations (reconnect/drop/reset) are async posts to each session's strand.
// ---------------------------------------------------------------------------
std::string PoolManager::handleControlCommand(const std::string& line) {
    // `json` is the namespace-level alias (mkpool::json) from zmq_client.hpp.
    std::vector<std::string> tok;
    { std::istringstream iss(line); std::string w; while (iss >> w) tok.push_back(w); }
    const std::string cmd = tok.empty() ? std::string{} : tok[0];

    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // One consistent read of a session: identity from its published snapshot,
    // fast-moving numbers from its live atomics. Gathered once under the shard
    // read-lock so each command works off a stable view.
    struct Row {
        std::uint64_t id{0};
        std::shared_ptr<const ClientSession::StatSnapshot> snap;  // identity; null pre-subscribe
        double        diff{0.0}, hr1{0.0}, hr5{0.0};
        std::uint64_t acc{0}, rej{0};
        std::int64_t  last_share{0};
    };

    auto row_json = [now](const Row& r) {
        const auto& s = r.snap;
        json j{{"id", r.id}};
        if (s) {
            j["ip"] = s->ip; j["address"] = s->worker_address; j["worker"] = s->worker_name;
            j["user_agent"] = s->user_agent; j["protocol"] = s->protocol; j["miner_id"] = s->miner_id;
            j["tier_port"] = s->tier_port;
            j["connected_secs"] = (s->connected_unix > 0 && now >= s->connected_unix) ? (now - s->connected_unix) : 0;
            j["authorized"] = s->authorized; j["subscribed"] = s->subscribed;
        }
        j["difficulty"] = r.diff;
        j["shares_accepted"] = r.acc; j["shares_rejected"] = r.rej;
        j["hashrate_1m"] = r.hr1; j["hashrate_5m"] = r.hr5;
        j["idle_secs"] = (r.last_share > 0 && now >= r.last_share) ? (now - r.last_share) : -1;
        return j;
    };

    auto read_row = [](std::uint64_t id, const std::shared_ptr<ClientSession>& s) {
        Row r;
        r.id = id;
        r.snap = s->statSnapshot();
        r.diff = s->liveDifficulty();
        r.hr1 = s->liveHashrate1m();
        r.hr5 = s->liveHashrate5m();
        r.acc = s->liveSharesAccepted();
        r.rej = s->liveSharesRejected();
        r.last_share = s->liveLastShareUnix();
        return r;
    };

    auto collect = [this, &read_row]() {
        std::vector<Row> out;
        for (auto& sh : shards_) {
            std::shared_lock lk(sh->mu);
            out.reserve(out.size() + sh->map.size());
            for (auto& [id, s] : sh->map) out.push_back(read_row(id, s));
        }
        return out;
    };

    if (cmd.empty())     return json{{"error", "empty command; try 'help'"}}.dump();
    if (cmd == "ping")   return json{{"result", "pong"}}.dump();

    if (cmd == "help") {
        return json{{"commands", {
            "ping", "help", "version", "uptime", "stats", "clients", "workers", "users",
            "getclient <id>", "getuser <address>", "getworker <address.worker>",
            "userclients <address>", "workerclients <address.worker>",
            "loglevel [trace|debug|info|warn|error|off]",
            "reconnect [host port [wait]]", "reconnclient <id> [host port [wait]]",
            "dropclient <id>", "dropall", "resetshares", "blacklistreload", "healthcheck",
            "role", "upstream"}}}.dump();
    }

    if (cmd == "version") {
        json coins = json::array();
        for (auto& rt : coins_) coins.push_back(rt.cfg.name);
        return json{{"engine", "mkpool"}, {"build", __DATE__ " " __TIME__}, {"coins", coins}}.dump();
    }

    // Scale-out roles: report the process role and each coin's uplink status.
    if (cmd == "role" || cmd == "upstream") {
        json j{{"role", role_to_string(global_.role)}};
        json ups = json::array();
        for (auto& rt : coins_) {
            if (!rt.upstream) continue;
            auto st = rt.upstream->status();
            json links = json::array();
            for (auto& l : st.links) {
                links.push_back(json{
                    {"link", l.index}, {"endpoint", l.endpoint}, {"connected", l.connected},
                    {"authorized", l.authorized}, {"primary", l.primary}, {"difficulty", l.difficulty},
                    {"extranonce1", l.extranonce1}, {"en2_size", l.en2_size}, {"jobs", l.jobs},
                    {"shares_forwarded", l.shares_forwarded}, {"shares_accepted", l.shares_accepted},
                    {"shares_rejected", l.shares_rejected}, {"reconnects", l.reconnects},
                    {"sessions", l.sessions}});
            }
            ups.push_back(json{
                {"coin", rt.cfg.name}, {"mode", st.mode}, {"primary", st.primary},
                {"total_sessions", st.total_sessions}, {"links", links}});
        }
        j["upstreams"] = ups;
        json redir = json::array();
        for (auto& rt : coins_) {
            if (!rt.redirectPolicy) continue;
            json backends = json::array();
            for (auto& h : rt.redirectPolicy->health())
                backends.push_back(json{{"host", h.host}, {"port", h.port}, {"up", h.up},
                                        {"latency_ms", h.latency_ms}});
            redir.push_back(json{{"coin", rt.cfg.name}, {"listeners", rt.relayListeners.size()},
                                 {"backends", backends}});
        }
        if (!redir.empty()) j["redirectors"] = redir;
        return j.dump();
    }

    if (cmd == "uptime") {
        return json{{"uptime_seconds", now - started_unix_}, {"started_unix", started_unix_}}.dump();
    }

    if (cmd == "loglevel") {
        if (tok.size() >= 2) {
            auto lvl = spdlog::level::from_str(tok[1]);
            if (lvl == spdlog::level::off && tok[1] != "off")
                return json{{"error", "unknown level '" + tok[1] + "'"}}.dump();
            spdlog::set_level(lvl);
            spdlog::info("[Control] log level changed to {}", tok[1]);
        }
        auto sv = spdlog::level::to_string_view(spdlog::get_level());
        return json{{"loglevel", std::string(sv.data(), sv.size())}}.dump();
    }

    if (cmd == "stats" || cmd == "poolstats") {
        auto snaps = collect();
        std::size_t authorized = 0;
        double pool_hr_1m = 0.0, pool_hr_5m = 0.0;
        for (auto& r : snaps) {
            if (r.snap && r.snap->authorized) ++authorized;
            pool_hr_1m += r.hr1;
            pool_hr_5m += r.hr5;
        }
        json coins = json::array();
        for (auto& rt : coins_) {
            json cj{{"name", rt.cfg.name}};
            if (rt.stratifier) {
                if (auto j = rt.stratifier->latestJob()) {
                    cj["template_height"]   = j->height;
                    cj["template_prevhash"] = j->previousblockhash;
                }
                cj["best_share_round"] = rt.stratifier->best_share_round();
            }
            json ports = json::array();
            for (auto& t : rt.cfg.stratumTiers) ports.push_back(t.port);
            cj["tier_ports"] = ports;
            coins.push_back(cj);
        }
        return json{{"connections", snaps.size()}, {"authorized", authorized},
                    {"hashrate_1m", pool_hr_1m}, {"hashrate_5m", pool_hr_5m},
                    {"uptime_seconds", now - started_unix_}, {"coins", coins}}.dump();
    }

    if (cmd == "clients") {
        auto snaps = collect();
        json arr = json::array();
        for (auto& r : snaps) arr.push_back(row_json(r));
        return json{{"count", arr.size()}, {"clients", arr}}.dump();
    }

    if (cmd == "getclient") {
        if (tok.size() < 2) return json{{"error", "usage: getclient <id>"}}.dump();
        std::uint64_t id = std::strtoull(tok[1].c_str(), nullptr, 10);
        auto s = sessionById(id);
        if (!s) return json{{"error", "no such client"}}.dump();
        return row_json(read_row(id, s)).dump();
    }

    if (cmd == "workers") {
        auto snaps = collect();
        std::map<std::string, json> g;
        for (auto& r : snaps) {
            if (!r.snap) continue;   // pre-subscribe connection has no worker identity yet
            const std::string key = r.snap->worker_address + "." + r.snap->worker_name;
            auto& e = g[key];
            if (e.is_null()) e = json{{"worker", key}, {"connections", 0},
                                      {"shares_accepted", 0}, {"shares_rejected", 0},
                                      {"hashrate_1m", 0.0}, {"hashrate_5m", 0.0}};
            e["connections"]     = e["connections"].get<int>() + 1;
            e["shares_accepted"] = e["shares_accepted"].get<std::uint64_t>() + r.acc;
            e["shares_rejected"] = e["shares_rejected"].get<std::uint64_t>() + r.rej;
            e["hashrate_1m"]     = e["hashrate_1m"].get<double>() + r.hr1;
            e["hashrate_5m"]     = e["hashrate_5m"].get<double>() + r.hr5;
        }
        json arr = json::array();
        for (auto& [k, v] : g) arr.push_back(v);
        return json{{"count", arr.size()}, {"workers", arr}}.dump();
    }

    if (cmd == "users") {
        auto snaps = collect();
        std::map<std::string, json> g;
        for (auto& r : snaps) {
            if (!r.snap) continue;
            auto& e = g[r.snap->worker_address];
            if (e.is_null()) e = json{{"address", r.snap->worker_address}, {"connections", 0},
                                      {"workers", json::object()},
                                      {"shares_accepted", 0}, {"shares_rejected", 0},
                                      {"hashrate_1m", 0.0}, {"hashrate_5m", 0.0}};
            e["connections"]        = e["connections"].get<int>() + 1;
            e["workers"][r.snap->worker_name] = e["workers"].value(r.snap->worker_name, 0) + 1;
            e["shares_accepted"]    = e["shares_accepted"].get<std::uint64_t>() + r.acc;
            e["shares_rejected"]    = e["shares_rejected"].get<std::uint64_t>() + r.rej;
            e["hashrate_1m"]        = e["hashrate_1m"].get<double>() + r.hr1;
            e["hashrate_5m"]        = e["hashrate_5m"].get<double>() + r.hr5;
        }
        json arr = json::array();
        for (auto& [k, v] : g) arr.push_back(v);
        return json{{"count", arr.size()}, {"users", arr}}.dump();
    }

    if (cmd == "getuser") {
        if (tok.size() < 2) return json{{"error", "usage: getuser <address>"}}.dump();
        auto snaps = collect();
        json workers = json::object();
        std::uint64_t acc = 0, rej = 0; int conns = 0;
        for (auto& r : snaps) if (r.snap && r.snap->worker_address == tok[1]) {
            ++conns; acc += r.acc; rej += r.rej;
            workers[r.snap->worker_name] = workers.value(r.snap->worker_name, 0) + 1;
        }
        if (conns == 0) return json{{"error", "no connections for that address"}}.dump();
        return json{{"address", tok[1]}, {"connections", conns}, {"workers", workers},
                    {"shares_accepted", acc}, {"shares_rejected", rej}}.dump();
    }

    if (cmd == "getworker") {
        if (tok.size() < 2) return json{{"error", "usage: getworker <address.worker>"}}.dump();
        const auto dot = tok[1].rfind('.');
        const std::string addr = dot == std::string::npos ? tok[1] : tok[1].substr(0, dot);
        const std::string wk   = dot == std::string::npos ? std::string{} : tok[1].substr(dot + 1);
        auto snaps = collect();
        json arr = json::array(); std::uint64_t acc = 0, rej = 0;
        for (auto& r : snaps) if (r.snap && r.snap->worker_address == addr && r.snap->worker_name == wk) {
            arr.push_back(row_json(r)); acc += r.acc; rej += r.rej;
        }
        if (arr.empty()) return json{{"error", "no such worker"}}.dump();
        return json{{"worker", tok[1]}, {"connections", arr.size()},
                    {"shares_accepted", acc}, {"shares_rejected", rej}, {"sessions", arr}}.dump();
    }

    if (cmd == "userclients") {
        if (tok.size() < 2) return json{{"error", "usage: userclients <address>"}}.dump();
        auto snaps = collect();
        json ids = json::array();
        for (auto& r : snaps)
            if (r.snap && r.snap->worker_address == tok[1]) ids.push_back(r.id);
        return json{{"address", tok[1]}, {"count", ids.size()}, {"client_ids", ids}}.dump();
    }

    if (cmd == "workerclients") {
        if (tok.size() < 2) return json{{"error", "usage: workerclients <address.worker>"}}.dump();
        const auto dot = tok[1].rfind('.');
        const std::string addr = dot == std::string::npos ? tok[1] : tok[1].substr(0, dot);
        const std::string wk   = dot == std::string::npos ? std::string{} : tok[1].substr(dot + 1);
        auto snaps = collect();
        json ids = json::array();
        for (auto& r : snaps)
            if (r.snap && r.snap->worker_address == addr && r.snap->worker_name == wk) ids.push_back(r.id);
        return json{{"worker", tok[1]}, {"count", ids.size()}, {"client_ids", ids}}.dump();
    }

    // ---- mutating commands ----
    if (cmd == "reconnect") {
        const std::string host = tok.size() > 1 ? tok[1] : std::string{};
        const std::string port = tok.size() > 2 ? tok[2] : std::string{};
        const int wait = tok.size() > 3 ? std::atoi(tok[3].c_str()) : 0;
        std::size_t n = 0;
        for (auto& sh : shards_) {
            std::shared_lock lk(sh->mu);
            for (auto& [id, s] : sh->map) { s->requestReconnect(host, port, wait); ++n; }
        }
        spdlog::warn("[Control] reconnect -> {} sessions (host='{}' port='{}' wait={})",
                     n, host, port, wait);
        return json{{"result", "ok"}, {"reconnected", n}}.dump();
    }

    if (cmd == "reconnclient") {
        if (tok.size() < 2) return json{{"error", "usage: reconnclient <id> [host port [wait]]"}}.dump();
        std::uint64_t id = std::strtoull(tok[1].c_str(), nullptr, 10);
        auto s = sessionById(id);
        if (!s) return json{{"error", "no such client"}}.dump();
        const std::string host = tok.size() > 2 ? tok[2] : std::string{};
        const std::string port = tok.size() > 3 ? tok[3] : std::string{};
        const int wait = tok.size() > 4 ? std::atoi(tok[4].c_str()) : 0;
        s->requestReconnect(host, port, wait);
        return json{{"result", "ok"}, {"id", id}}.dump();
    }

    if (cmd == "dropclient") {
        if (tok.size() < 2) return json{{"error", "usage: dropclient <id>"}}.dump();
        std::uint64_t id = std::strtoull(tok[1].c_str(), nullptr, 10);
        auto s = sessionById(id);
        if (!s) return json{{"error", "no such client"}}.dump();
        s->shutdown();
        spdlog::warn("[Control] dropped client {}", id);
        return json{{"result", "ok"}, {"id", id}}.dump();
    }

    if (cmd == "dropall") {
        std::size_t n = 0;
        for (auto& sh : shards_) {
            std::vector<std::shared_ptr<ClientSession>> snap;
            { std::shared_lock lk(sh->mu); for (auto& [id, s] : sh->map) snap.push_back(s); }
            for (auto& s : snap) { s->shutdown(); ++n; }
        }
        spdlog::warn("[Control] dropall -> {} sessions", n);
        return json{{"result", "ok"}, {"dropped", n}}.dump();
    }

    if (cmd == "resetshares") {
        // Reset the pool best-share-of-round plus every session's counters.
        for (auto& rt : coins_) if (rt.stratifier) rt.stratifier->reset_best_share();
        std::size_t n = 0;
        for (auto& sh : shards_) {
            std::shared_lock lk(sh->mu);
            for (auto& [id, s] : sh->map) { s->resetShareCounters(); ++n; }
        }
        return json{{"result", "ok"}, {"reset", n}}.dump();
    }

    if (cmd == "blacklistreload") {
        Blacklist::instance().reload();
        return json{{"result", "ok"}}.dump();
    }

    if (cmd == "healthcheck") {
        json coins = json::array();
        bool ok = true;
        for (auto& rt : coins_) {
            const bool has_tpl = rt.stratifier && rt.stratifier->latestJob() != nullptr;
            if (!has_tpl) ok = false;
            coins.push_back(json{{"name", rt.cfg.name}, {"template", has_tpl}});
        }
        return json{{"status", ok ? "ok" : "degraded"}, {"coins", coins}}.dump();
    }

    return json{{"error", "unknown command '" + cmd + "'; try 'help'"}}.dump();
}

} // namespace mkpool
