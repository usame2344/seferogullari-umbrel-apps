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
// File:        config.hpp
// Description: Multi-coin pool configuration schema and JSON parsing (pools, tiers, TLS, SV2).
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace mkpool {

using json = nlohmann::json;

enum class ChainKind { Bitcoin, BitcoinCash, BitcoinII, DigiByte, eCash, Zcash, Litecoin, Dogecoin, BitcoinCashII };

// Process operating role. `Pool` (the default) is the original solo engine and
// is completely unaffected by any of the scale-out code: when the role is Pool,
// none of the relay subsystems are constructed. The other roles turn mkpool into
// a distribution layer:
//   Proxy       - outbound Stratum client to an upstream pool; re-serves work to
//                 downstream miners with local vardiff + extranonce subdivision.
//   Redirector  - accepts a miner then immediately client.reconnect()s it onto a
//                 rotation of real backends, then closes.
//   Passthrough - (stage 2) thin multiplexer of downstream miners over one uplink
//                 to an mkpool origin that keeps per-miner accounting.
//   Node        - (stage 2) passthrough plus a local node for block submission.
enum class PoolRole { Pool, Proxy, Passthrough, Node, Redirector };

[[nodiscard]] inline PoolRole parse_role(std::string_view s) noexcept {
    if (s == "proxy")       return PoolRole::Proxy;
    if (s == "passthrough") return PoolRole::Passthrough;
    if (s == "node")        return PoolRole::Node;
    if (s == "redirector")  return PoolRole::Redirector;
    return PoolRole::Pool; // default and unknown -> solo, never a surprise role
}

[[nodiscard]] inline const char* role_to_string(PoolRole r) noexcept {
    switch (r) {
        case PoolRole::Proxy:       return "proxy";
        case PoolRole::Passthrough: return "passthrough";
        case PoolRole::Node:        return "node";
        case PoolRole::Redirector:  return "redirector";
        case PoolRole::Pool:        return "pool";
    }
    return "pool";
}

struct ZmqTopics {
    std::vector<std::string> hashblock;
    std::vector<std::string> rawblock;
    std::vector<std::string> rawtx;
};

struct AuxConfig {
    bool enabled{false};
    std::string rpcHost{"127.0.0.1"};
    std::string rpcPort;
    std::string rpcUser;
    std::string rpcPassword;
    std::string payoutAddress;
    // Operator donation taken from the aux (DOGE) coinbase, same model as the
    // parent chain. Pays the operator address out of every aux block we find.
    double      donationPercent{0.0};
    std::string donationAddress;
};

// One upstream Stratum endpoint the proxy connects out to. Endpoints are tried
// in priority order (lower number first); on transport failure the proxy fails
// over to the next and a watchdog restores the preferred one when it recovers.
struct UpstreamEndpoint {
    std::string   host;
    std::uint16_t port{0};
    bool          tls{false};       // wrap the uplink in TLS (verify off by default; see verifyPeer)
    bool          verifyPeer{false};// when tls, verify the upstream certificate chain/hostname
    std::string   user;             // mining.authorize username (usually the pool's payout addr[.worker])
    std::string   pass{"x"};        // mining.authorize password
    int           priority{0};      // lower = preferred (failover order / sticky primary pick)
    std::uint32_t weight{1};        // active/active load share (higher = more downstream miners)
};

// Proxy-role settings for a coin. Only consulted when global.role == Proxy.
struct UpstreamConfig {
    std::vector<UpstreamEndpoint> endpoints;
    // How downstream miners are distributed across the endpoints:
    //   "failover" (default) - all miners on one primary link; every other link
    //                          is kept connected+authorized as a HOT STANDBY, so
    //                          a primary failure migrates miners in well under a
    //                          second with near-zero lost work (sticky primary:
    //                          it only moves on failure, never churns back).
    //   "activeactive"       - miners are spread across ALL ready links by weight
    //                          (weighted least-connections); a link failure
    //                          redistributes its miners to the survivors.
    // Every link connects and stays warm in both modes.
    std::string mode{"failover"};
    // When true, the upstream endpoints speak Stratum V2 (an SV2 pool) and the
    // proxy connects out with an SV2 extended channel, translating downstream V1
    // miners onto it. Default false => the classic V1 upstream. This is the
    // "V1 miners -> SV2 pool" direction of the bidirectional translator.
    bool sv2{false};
    // Bytes of the upstream extranonce2 space reserved as a per-downstream slot
    // id. Downstream miners receive (upstream_en2_size - nonceBytes) bytes of
    // their own extranonce2, and the proxy reconstructs the full upstream
    // extranonce2 = slot || downstream_en2 on submit. 2 bytes = 65536 slots.
    std::uint8_t  nonceBytes{2};
    // Seconds of upstream silence before the proxy treats the link as dead and
    // reconnects/fails over. 0 -> default (90s).
    std::uint16_t idleTimeoutSeconds{90};
    // Reconnect backoff bounds (seconds).
    std::uint16_t reconnectMinSeconds{2};
    std::uint16_t reconnectMaxSeconds{30};
    [[nodiscard]] bool enabled() const noexcept { return !endpoints.empty(); }
};

// Redirector-role settings for a coin. Only consulted when
// global.role == Redirector.
struct RedirectConfig {
    struct Target { std::string host; std::uint16_t port{0}; std::uint32_t weight{1}; };
    std::vector<Target> targets;
    // round-robin | random | latency (steer to the fastest healthy backend).
    std::string   strategy{"round-robin"};
    // Seconds the miner should wait before reconnecting (client.reconnect arg).
    std::uint16_t wait{0};
    // Active backend health probing: periodically TCP-connect each target,
    // record up/down + latency, and only steer miners to healthy backends
    // (preferring the fastest under strategy="latency"). Off => every target is
    // assumed healthy (plain round-robin/random).
    bool          healthCheck{false};
    std::uint16_t healthCheckIntervalSeconds{10};
    std::uint16_t healthCheckTimeoutMs{1000};
    [[nodiscard]] bool enabled() const noexcept { return !targets.empty(); }
};

// Cluster (stage-2 scale-out) settings for a coin. Covers BOTH directions of the
// mkpool-native cluster trunk protocol; which half is consulted depends on the
// process role:
//   - Origin (global.role == Pool): the `ingest*` fields turn on a trunk acceptor
//     that bridges each multiplexed downstream miner to this coin's own local
//     stratum port, so the solo engine serves them as ordinary local miners with
//     no change to any solo code. Inert unless `ingestPort` is set.
//   - Passthrough/Node (global.role == Passthrough/Node): the `origin*` fields
//     say where to trunk this coin's downstream miners. Inert unless `originHost`
//     and `originPort` are set.
// The whole struct is left at defaults (all off) unless a "cluster" key is present
// in the coin config, so pool operators who never use it pay nothing.
struct ClusterConfig {
    // ---- Origin ingest (pool role) ----
    std::string   ingestAddress{"0.0.0.0"};
    std::uint16_t ingestPort{0};        // 0 => ingest disabled for this coin
    bool          ingestTls{false};     // terminate TLS on the trunk acceptor
    std::string   forwardHost{"127.0.0.1"};
    std::uint16_t forwardPort{0};       // 0 => this coin's stratumListenPort
    // ZMQ rawblock endpoint of the origin's local node. When set, every raw block
    // the origin's node accepts is streamed down NODE trunks for local submission.
    // Empty => node block relay off (node trunks still work as passthroughs).
    std::string   rawblockZmq;

    // ---- Passthrough/Node uplink ----
    std::string   originHost;
    std::uint16_t originPort{0};        // 0 => uplink disabled
    bool          originTls{false};
    bool          originVerifyPeer{false};
    std::uint16_t trunks{1};            // parallel trunk connections to spread miners over

    // ---- Shared ----
    std::string   token;                // shared secret required in the trunk hello (both ends)
    std::uint16_t idleTimeoutSeconds{90};

    [[nodiscard]] bool ingest_on() const noexcept { return ingestPort != 0; }
    [[nodiscard]] bool uplink_on() const noexcept { return !originHost.empty() && originPort != 0; }
};

struct CoinConfig {
    std::string name{"BTC"};
    ChainKind   chain{ChainKind::Bitcoin};

    // RPC
    std::string rpcHost{"127.0.0.1"};
    std::string rpcPort{"8332"};
    std::string rpcUser;
    std::string rpcPassword;

    // Optional failover nodes. Tried in order after the primary when the
    // active node stops responding; a watchdog switches back once the primary
    // recovers. Empty (the default) => single-node behaviour, unchanged.
    struct RpcNode { std::string host; std::string port; std::string user; std::string pass; };
    std::vector<RpcNode> rpcFallbacks;

    // Aux merged mining
    AuxConfig aux;

    // ZMQ
    bool useZMQ{true};
    std::string zmqAddress{"tcp://127.0.0.1:28332"}; // legacy single
    ZmqTopics zmq;

    // Stratum
    std::string  stratumListenAddress{"0.0.0.0"};
    std::uint16_t stratumListenPort{3333};
    std::uint16_t stratumTlsPort{0}; // 0 = disabled
    std::uint16_t stratumMaxConnectionsPerIP{1000};
    std::uint16_t stratumV2Port{0}; // 0 = disabled
    double        stratumV2Difficulty{1024.0}; // fixed share difficulty for the v2 channel
    bool          stratumV2EmptyBlocks{false};  // false = full blocks (mainnet, collects fees); true = coinbase-only subsidy-only (testnet only)
    bool          sv2NoiseRequired{false};
    std::string   sv2AuthorityKey;

    // Stratum listener tiers. Each tier is its own TCP port with its own
    // fixed difficulty (or vardiff range). Miners pick the port matching
    // their hashrate class; the pool never reshuffles their diff. Aggregator
    // services (NiceHash / MiningRigRentals) are gated to opt-in high-diff
    // tiers via allowNiceHash to prevent share floods.
    struct StratumTier {
        std::uint16_t port{0};
        std::string   label;                  // for logs ("vardiff", "asic-1M", ...)
        double        startingDifficulty{65536.0};
        // Vardiff: when true, on_share() retargets between [vardiffMin,vardiffMax].
        // When false (default) the tier is fixed at startingDifficulty for the
        // whole session, no matter what the miner suggests.
        bool          vardiffEnabled{false};
        double        vardiffMin{1024.0};        // 1K
        double        vardiffMax{10'000'000.0};  // 10M
        // Aggregator gate. NiceHash / MRR identify themselves in mining.subscribe
        // params[0]. On tiers with allowNiceHash=false such UAs are rejected at
        // subscribe time. Set true on the dedicated high-diff aggregator ports.
        bool          allowNiceHash{false};
        // When true the listener terminates TLS (OpenSSL) before the Stratum V1
        // line protocol. Requires global.tls cert/key to be configured. Only
        // valid on V1 tiers; Stratum V2 has its own Noise encryption.
        bool          tls{false};
        // When true this tier is a Stratum V2 (Noise) listener rather than V1.
        // Combined with vardiffEnabled=false this yields a fixed-difficulty SV2
        // port. The legacy single v2Port sets this too (see PoolManager). The
        // handshake path keys exclusive-V2 detection off this flag, not the label.
        bool          sv2{false};
    };
    std::vector<StratumTier> stratumTiers;

    // Populated per-Connector by PoolManager: each ClientSession reads its
    // tier settings from coin_.activeTier (port / startingDifficulty /
    // vardiff range / NH gate).
    StratumTier activeTier;

    // Vardiff math tuning (only matters on vardiff-enabled tiers). Kept at
    // coin scope because the controller dynamics are the same on every port.
    double        targetSharesPerMinute{12.0}; // -> 0.2 Hz
    double        vardiffTauSeconds{30.0};
    std::uint16_t blockPollInterval{10};

    // Coinbase / payouts
    std::string coinbaseSignature{"/mkpool.com/"};
    double      donationPercent{1.0};
    std::string donationAddress;

    // BIP310 (Bitcoin only; BCH ignores it)
    bool          enableVersionRolling{true};
    std::uint32_t versionRollingMask{0x1fffe000u};

    // Job window
    std::uint16_t jobWindowSize{32};

    // Block submission
    std::vector<std::string> additionalSubmitEndpoints; // host:port pairs (optional)

    // Scale-out roles (all inert unless global.role selects the matching role).
    // Proxy: where this coin's downstream miners' work comes from upstream.
    UpstreamConfig upstream;
    // Redirector: backends this coin's listeners bounce miners to.
    RedirectConfig redirect;
    // Passthrough/Node (origin ingest + edge uplink). See ClusterConfig.
    ClusterConfig  cluster;

    // Helpers
    [[nodiscard]] std::string primary_zmq_hashblock() const {
        return zmq.hashblock.empty() ? zmqAddress : zmq.hashblock.front();
    }
};

struct TlsConfigJson {
    std::string certFile;
    std::string keyFile;
    std::string dhparamsFile;
};

struct RateLimitJson {
    // DDoS-only defaults. Must not throttle legitimate mining traffic (incl.
    // large MRR/NiceHash proxy IPs aggregating many rigs).
    std::uint32_t maxConnectionsPerIP{50000};
    std::uint32_t acceptBurst{10000};
    double        acceptRefillPerSec{1000.0};
    std::uint32_t invalidShareBanThreshold{10000};
    std::uint32_t invalidShareWindowSec{300};
    std::uint32_t banDurationSec{3600};
};

struct GlobalConfig {
    // Process operating role. "pool" (default) is the solo engine; the scale-out
    // roles are opt-in and inert otherwise. Parsed into `role` below.
    PoolRole     role{PoolRole::Pool};

    std::string  metricsListenAddress{"0.0.0.0"};
    std::uint16_t metricsListenPort{9090};

    std::string logPath{"logs"};
    std::uint16_t logLevel{2};

    std::string databaseHost{"127.0.0.1"};
    std::uint16_t databasePort{5432};
    std::string databaseName{"mkpool"};
    std::string databaseUser{"mkpool_user"};
    std::string databasePassword;

    std::uint16_t ioThreads{0};  // 0 = hardware_concurrency
    std::uint16_t sessionShards{64};

    bool debugMode{false};

    // Runtime control/admin socket. Empty here => mkpool derives a per
    // instance default (/run/mkpool/<instance>.sock). Set to "off"/"none"/"-"
    // to disable the control socket entirely.
    std::string controlSocket;

    // Drop a connected miner that has not submitted an accepted share for
    // this many seconds (dead-worker reap). 0 (default) disables the sweep.
    // Use a generous value; high-diff miners can legitimately be quiet a while.
    std::uint32_t idleDropSeconds{0};

    // On SIGTERM/SIGINT, gracefully drain miners over this many seconds instead
    // of dropping them all at once: stop accepting, migrate V1 miners with a
    // staggered client.reconnect, then close any stragglers and exit. 0
    // (default) keeps the immediate hard stop. Pairs with a
    // start-new-before-stop-old (SO_REUSEPORT) restart for a near-zero-downtime
    // upgrade. Keep it well under systemd's TimeoutStopSec.
    std::uint32_t restartDrainSeconds{0};

    TlsConfigJson tls;
    RateLimitJson rateLimit;
};

struct Config {
    std::string activeCoin{"BTC"};
    GlobalConfig global;
    std::map<std::string, CoinConfig> coins;

    [[nodiscard]] const CoinConfig& getActiveCoinConfig() const {
        auto it = coins.find(activeCoin);
        if (it == coins.end())
            throw std::runtime_error("active coin '" + activeCoin + "' missing");
        return it->second;
    }

    static std::optional<Config> fromJsonFile(const std::string& path) {
        if (!std::filesystem::exists(path)) return std::nullopt;
        std::ifstream f(path);
        if (!f) throw std::runtime_error("cannot open config: " + path);
        json j;
        f >> j;
        return fromJson(j);
    }

    static Config parse(int /*argc*/, char* /*argv*/[]) {
        return fromJsonFile("config.json").value_or(Config{});
    }

    static Config fromJson(const json& j) {
        Config c;
        c.activeCoin = j.value("activeCoin", "BTC");

        if (j.contains("global")) {
            const auto& g = j["global"];
            c.global.role                  = parse_role(g.value("role", std::string{"pool"}));
            c.global.metricsListenAddress  = g.value("metricsListenAddress", c.global.metricsListenAddress);
            c.global.metricsListenPort     = g.value("metricsListenPort", c.global.metricsListenPort);

            c.global.logPath               = g.value("logPath", c.global.logPath);
            c.global.logLevel              = g.value("logLevel", c.global.logLevel);
            c.global.databaseHost          = g.value("databaseHost", c.global.databaseHost);
            c.global.databasePort          = g.value("databasePort", c.global.databasePort);
            c.global.databaseName          = g.value("databaseName", c.global.databaseName);
            c.global.databaseUser          = g.value("databaseUser", c.global.databaseUser);
            c.global.databasePassword      = g.value("databasePassword", c.global.databasePassword);
            c.global.ioThreads             = g.value("ioThreads", c.global.ioThreads);
            c.global.sessionShards         = g.value("sessionShards", c.global.sessionShards);
            c.global.debugMode             = g.value("debugMode", c.global.debugMode);
            c.global.controlSocket         = g.value("controlSocket", c.global.controlSocket);
            c.global.idleDropSeconds       = g.value("idleDropSeconds", c.global.idleDropSeconds);
            c.global.restartDrainSeconds   = g.value("restartDrainSeconds", c.global.restartDrainSeconds);

            if (g.contains("tls") && g["tls"].is_object()) {
                const auto& t = g["tls"];
                c.global.tls.certFile     = t.value("certFile", "");
                c.global.tls.keyFile      = t.value("keyFile", "");
                c.global.tls.dhparamsFile = t.value("dhparamsFile", "");
            }
            if (g.contains("rateLimit") && g["rateLimit"].is_object()) {
                const auto& r = g["rateLimit"];
                c.global.rateLimit.maxConnectionsPerIP     = r.value("maxConnectionsPerIP", c.global.rateLimit.maxConnectionsPerIP);
                c.global.rateLimit.acceptBurst             = r.value("acceptBurst", c.global.rateLimit.acceptBurst);
                c.global.rateLimit.acceptRefillPerSec      = r.value("acceptRefillPerSec", c.global.rateLimit.acceptRefillPerSec);
                c.global.rateLimit.invalidShareBanThreshold = r.value("invalidShareBanThreshold", c.global.rateLimit.invalidShareBanThreshold);
                c.global.rateLimit.invalidShareWindowSec    = r.value("invalidShareWindowSec", c.global.rateLimit.invalidShareWindowSec);
                c.global.rateLimit.banDurationSec           = r.value("banDurationSec", c.global.rateLimit.banDurationSec);
            }
        }

        if (j.contains("coins") && j["coins"].is_object()) {
            for (auto it = j["coins"].begin(); it != j["coins"].end(); ++it) {
                CoinConfig cc = parseCoin(it.value());
                cc.name = it.key();
                c.coins[it.key()] = std::move(cc);
            }
        }
        if (c.coins.empty()) {
            // ensure default BTC coin
            c.coins["BTC"] = CoinConfig{};
        }
        if (!c.coins.count(c.activeCoin)) {
            throw std::runtime_error("active coin '" + c.activeCoin + "' not configured");
        }
        return c;
    }

    static CoinConfig parseCoin(const json& j) {
        CoinConfig cc;
        cc.rpcHost            = j.value("rpcHost", cc.rpcHost);
        cc.rpcPort            = j.value("rpcPort", cc.rpcPort);
        cc.rpcUser            = j.value("rpcUser", cc.rpcUser);
        cc.rpcPassword        = j.value("rpcPassword", cc.rpcPassword);

        // Optional failover nodes. Each entry may specify its own creds;
        // omitted creds default to the primary's. Port accepts string or number.
        if (j.contains("rpcFallbacks") && j["rpcFallbacks"].is_array()) {
            auto asPort = [](const json& n, const char* key) -> std::string {
                if (!n.contains(key)) return {};
                return n[key].is_string() ? n[key].get<std::string>()
                                          : std::to_string(n[key].get<long long>());
            };
            for (const auto& n : j["rpcFallbacks"]) {
                if (!n.is_object()) continue;
                CoinConfig::RpcNode node;
                node.host = n.value("rpcHost", n.value("host", std::string{}));
                node.port = asPort(n, "rpcPort");
                if (node.port.empty()) node.port = asPort(n, "port");
                node.user = n.value("rpcUser", n.value("user", cc.rpcUser));
                node.pass = n.value("rpcPassword", n.value("pass", cc.rpcPassword));
                if (!node.host.empty() && !node.port.empty())
                    cc.rpcFallbacks.push_back(std::move(node));
            }
        }

        if (j.contains("aux") && j["aux"].is_object()) {
            const auto& a = j["aux"];
            cc.aux.enabled = a.value("enabled", true);
            cc.aux.rpcHost = a.value("rpcHost", cc.aux.rpcHost);
            cc.aux.rpcPort = a.value("rpcPort", cc.aux.rpcPort);
            cc.aux.rpcUser = a.value("rpcUser", cc.aux.rpcUser);
            cc.aux.rpcPassword = a.value("rpcPassword", cc.aux.rpcPassword);
            cc.aux.payoutAddress = a.value("payoutAddress", cc.aux.payoutAddress);
            cc.aux.donationPercent = a.value("donationPercent", cc.aux.donationPercent);
            cc.aux.donationAddress = a.value("donationAddress", cc.aux.donationAddress);
        }
        cc.useZMQ             = j.value("useZMQ", cc.useZMQ);
        cc.zmqAddress         = j.value("zmqAddress", cc.zmqAddress);
        if (j.contains("zmq") && j["zmq"].is_object()) {
            auto parseList = [](const json& n) -> std::vector<std::string> {
                std::vector<std::string> r;
                if (n.is_string()) r.push_back(n.get<std::string>());
                else if (n.is_array()) for (const auto& v : n)
                    if (v.is_string()) r.push_back(v.get<std::string>());
                return r;
            };
            const auto& z = j["zmq"];
            if (z.contains("hashblock")) cc.zmq.hashblock = parseList(z["hashblock"]);
            if (z.contains("rawblock"))  cc.zmq.rawblock  = parseList(z["rawblock"]);
            if (z.contains("rawtx"))     cc.zmq.rawtx     = parseList(z["rawtx"]);
        }
        // Try nested "stratum" block
        if (j.contains("stratum") && j["stratum"].is_object()) {
            const auto& s = j["stratum"];
            cc.stratumListenAddress       = s.value("listenAddress", s.value("stratumListenAddress", cc.stratumListenAddress));
            cc.stratumListenPort          = s.value("listenPort", s.value("stratumListenPort", cc.stratumListenPort));
            cc.stratumTlsPort             = s.value("tlsPort", s.value("stratumTlsPort", cc.stratumTlsPort));
            cc.stratumMaxConnectionsPerIP = s.value("maxConnectionsPerIP", s.value("stratumMaxConnectionsPerIP", cc.stratumMaxConnectionsPerIP));
            cc.blockPollInterval          = s.value("blockPollInterval", cc.blockPollInterval);
            cc.coinbaseSignature          = s.value("coinbaseSignature", cc.coinbaseSignature);
            cc.jobWindowSize              = s.value("jobWindowSize", cc.jobWindowSize);
            cc.stratumV2Port              = s.value("v2Port", s.value("stratumV2Port", cc.stratumV2Port));
            cc.stratumV2Difficulty        = s.value("v2Difficulty", cc.stratumV2Difficulty);
            cc.stratumV2EmptyBlocks       = s.value("v2EmptyBlocks", cc.stratumV2EmptyBlocks);
            cc.sv2NoiseRequired           = s.value("v2NoiseRequired", cc.sv2NoiseRequired);
            cc.sv2AuthorityKey            = s.value("v2AuthorityKey", cc.sv2AuthorityKey);
            
            if (s.contains("versionRolling") && s["versionRolling"].is_object()) {
                const auto& vr = s["versionRolling"];
                cc.enableVersionRolling   = vr.value("enable", cc.enableVersionRolling);
                if (vr.contains("mask") && vr["mask"].is_string()) {
                    try { cc.versionRollingMask = std::stoul(vr["mask"].get<std::string>(), nullptr, 16); }
                    catch (...) {}
                }
            } else {
                cc.enableVersionRolling   = s.value("enableVersionRolling", cc.enableVersionRolling);
                if (s.contains("versionRollingMask") && s["versionRollingMask"].is_string()) {
                    try { cc.versionRollingMask = std::stoul(s["versionRollingMask"].get<std::string>(), nullptr, 16); }
                    catch (...) {}
                }
            }
        } else {
            cc.stratumListenAddress       = j.value("stratumListenAddress", cc.stratumListenAddress);
            cc.stratumListenPort          = j.value("stratumListenPort", cc.stratumListenPort);
            cc.stratumTlsPort             = j.value("stratumTlsPort", cc.stratumTlsPort);
            cc.stratumMaxConnectionsPerIP = j.value("stratumMaxConnectionsPerIP", cc.stratumMaxConnectionsPerIP);
            cc.blockPollInterval          = j.value("blockPollInterval", cc.blockPollInterval);
            cc.coinbaseSignature          = j.value("coinbaseSignature", cc.coinbaseSignature);
            cc.jobWindowSize              = j.value("jobWindowSize", cc.jobWindowSize);
            cc.stratumV2Port              = j.value("stratumV2Port", cc.stratumV2Port);
            cc.stratumV2Difficulty        = j.value("stratumV2Difficulty", cc.stratumV2Difficulty);
            cc.stratumV2EmptyBlocks       = j.value("stratumV2EmptyBlocks", cc.stratumV2EmptyBlocks);
            cc.sv2NoiseRequired           = j.value("sv2NoiseRequired", cc.sv2NoiseRequired);
            cc.sv2AuthorityKey            = j.value("sv2AuthorityKey", cc.sv2AuthorityKey);
            cc.enableVersionRolling       = j.value("enableVersionRolling", cc.enableVersionRolling);
            if (j.contains("versionRollingMask") && j["versionRollingMask"].is_string()) {
                try { cc.versionRollingMask = std::stoul(j["versionRollingMask"].get<std::string>(), nullptr, 16); }
                catch (...) {}
            }
        }

        // Try nested "vardiff" block
        if (j.contains("vardiff") && j["vardiff"].is_object()) {
            const auto& vd = j["vardiff"];
            cc.targetSharesPerMinute      = vd.value("targetSharesPerMinute", cc.targetSharesPerMinute);
            cc.vardiffTauSeconds          = vd.value("tauSeconds", vd.value("vardiffTauSeconds", cc.vardiffTauSeconds));
        } else {
            cc.targetSharesPerMinute      = j.value("targetSharesPerMinute", cc.targetSharesPerMinute);
            cc.vardiffTauSeconds          = j.value("vardiffTauSeconds", cc.vardiffTauSeconds);
        }

        // Try nested "donation" block
        if (j.contains("donation") && j["donation"].is_object()) {
            const auto& d = j["donation"];
            d.contains("percentage") ? cc.donationPercent = d.value("percentage", cc.donationPercent) : cc.donationPercent = d.value("percent", cc.donationPercent);
            cc.donationAddress            = d.value("address", d.value("donationAddress", cc.donationAddress));
        } else {
            if (j.contains("donationPercentage"))
                cc.donationPercent = j.value("donationPercentage", cc.donationPercent);
            else
                cc.donationPercent = j.value("donationPercent", cc.donationPercent);
            cc.donationAddress            = j.value("donationAddress", cc.donationAddress);
        }
        if (j.contains("stratumTiers") && j["stratumTiers"].is_array()) {
            for (const auto& t : j["stratumTiers"]) {
                if (!t.is_object()) continue;
                CoinConfig::StratumTier st;
                st.port               = t.value("port", std::uint16_t{0});
                st.label              = t.value("label", std::string{});
                st.startingDifficulty = t.value("startingDifficulty", st.startingDifficulty);
                st.vardiffEnabled     = t.value("vardiffEnabled", st.vardiffEnabled);
                st.vardiffMin         = t.value("vardiffMin", st.vardiffMin);
                st.vardiffMax         = t.value("vardiffMax", st.vardiffMax);
                st.allowNiceHash      = t.value("allowNiceHash", st.allowNiceHash);
                st.tls                = t.value("tls", st.tls);
                st.sv2                = t.value("sv2", st.sv2);
                if (st.port == 0) continue;
                cc.stratumTiers.push_back(std::move(st));
            }
        }
        if (j.contains("additionalSubmitEndpoints") && j["additionalSubmitEndpoints"].is_array()) {
            for (const auto& s : j["additionalSubmitEndpoints"])
                if (s.is_string()) cc.additionalSubmitEndpoints.push_back(s.get<std::string>());
        }

        // ---- Scale-out: proxy upstream endpoints (inert unless role==proxy) ----
        // Reads a uint16 from obj[key] whether stored as a JSON number or string.
        auto asU16 = [](const json& obj, const char* key, std::uint16_t dflt) -> std::uint16_t {
            if (!obj.is_object() || !obj.contains(key)) return dflt;
            const auto& n = obj[key];
            if (n.is_number_unsigned()) return static_cast<std::uint16_t>(n.get<unsigned>());
            if (n.is_number_integer())  return static_cast<std::uint16_t>(n.get<long long>());
            if (n.is_string()) { try { return static_cast<std::uint16_t>(std::stoi(n.get<std::string>())); } catch (...) {} }
            return dflt;
        };
        if (j.contains("upstream") && j["upstream"].is_object()) {
            const auto& u = j["upstream"];
            cc.upstream.mode               = u.value("mode", cc.upstream.mode);
            cc.upstream.sv2                = u.value("sv2", false);
            cc.upstream.nonceBytes         = static_cast<std::uint8_t>(asU16(u, "nonceBytes", 2));
            if (cc.upstream.nonceBytes < 1) cc.upstream.nonceBytes = 1;
            if (cc.upstream.nonceBytes > 4) cc.upstream.nonceBytes = 4;
            cc.upstream.idleTimeoutSeconds  = asU16(u, "idleTimeoutSeconds", 90);
            cc.upstream.reconnectMinSeconds = asU16(u, "reconnectMinSeconds", 2);
            cc.upstream.reconnectMaxSeconds = asU16(u, "reconnectMaxSeconds", 30);
            const json* eps = nullptr;
            if (u.contains("endpoints") && u["endpoints"].is_array()) eps = &u["endpoints"];
            if (eps) {
                for (const auto& e : *eps) {
                    if (!e.is_object()) continue;
                    UpstreamEndpoint ep;
                    ep.host = e.value("host", std::string{});
                    ep.port = asU16(e, "port", 0);
                    // "host:port" shorthand splits into host + port.
                    if (ep.port == 0) {
                        const auto colon = ep.host.rfind(':');
                        if (colon != std::string::npos && colon + 1 < ep.host.size()) {
                            try { ep.port = static_cast<std::uint16_t>(std::stoi(ep.host.substr(colon + 1))); } catch (...) {}
                            ep.host.erase(colon);
                        }
                    }
                    ep.tls        = e.value("tls", false);
                    ep.verifyPeer = e.value("verifyPeer", false);
                    ep.user       = e.value("user", std::string{});
                    ep.pass       = e.value("pass", std::string{"x"});
                    // Explicit priority wins; equal/absent priorities keep config
                    // order (UpstreamClient sorts stably).
                    ep.priority   = e.value("priority", 0);
                    ep.weight     = e.value("weight", 1u);
                    if (ep.weight == 0) ep.weight = 1;
                    if (!ep.host.empty() && ep.port != 0)
                        cc.upstream.endpoints.push_back(std::move(ep));
                }
            }
        }

        // ---- Scale-out: redirector backends (inert unless role==redirector) ----
        if (j.contains("redirect") && j["redirect"].is_object()) {
            const auto& r = j["redirect"];
            cc.redirect.strategy = r.value("strategy", cc.redirect.strategy);
            cc.redirect.wait     = asU16(r, "wait", 0);
            cc.redirect.healthCheck                = r.value("healthCheck", cc.redirect.healthCheck);
            cc.redirect.healthCheckIntervalSeconds = asU16(r, "healthCheckIntervalSeconds", 10);
            cc.redirect.healthCheckTimeoutMs       = asU16(r, "healthCheckTimeoutMs", 1000);
            if (r.contains("targets") && r["targets"].is_array()) {
                for (const auto& t : r["targets"]) {
                    RedirectConfig::Target tg;
                    if (t.is_string()) {
                        std::string hp = t.get<std::string>();
                        const auto colon = hp.rfind(':');
                        if (colon != std::string::npos && colon + 1 < hp.size()) {
                            try { tg.port = static_cast<std::uint16_t>(std::stoi(hp.substr(colon + 1))); } catch (...) {}
                            tg.host = hp.substr(0, colon);
                        } else {
                            tg.host = hp;
                        }
                    } else if (t.is_object()) {
                        tg.host   = t.value("host", std::string{});
                        tg.port   = asU16(t, "port", 0);
                        tg.weight = t.value("weight", 1u);
                        if (tg.weight == 0) tg.weight = 1;
                    }
                    if (!tg.host.empty() && tg.port != 0)
                        cc.redirect.targets.push_back(std::move(tg));
                }
            }
        }

        // ---- Scale-out: cluster passthrough/node (inert unless role selects it) ----
        if (j.contains("cluster") && j["cluster"].is_object()) {
            const auto& cl = j["cluster"];
            cc.cluster.ingestAddress = cl.value("ingestAddress", cc.cluster.ingestAddress);
            cc.cluster.ingestPort    = asU16(cl, "ingestPort", 0);
            cc.cluster.ingestTls     = cl.value("ingestTls", false);
            cc.cluster.forwardHost   = cl.value("forwardHost", cc.cluster.forwardHost);
            cc.cluster.forwardPort   = asU16(cl, "forwardPort", 0);
            cc.cluster.rawblockZmq   = cl.value("rawblockZmq", std::string{});
            cc.cluster.originHost    = cl.value("originHost", cl.value("host", std::string{}));
            cc.cluster.originPort    = asU16(cl, "originPort", asU16(cl, "port", 0));
            // "originHost:port" shorthand.
            if (cc.cluster.originPort == 0) {
                const auto colon = cc.cluster.originHost.rfind(':');
                if (colon != std::string::npos && colon + 1 < cc.cluster.originHost.size()) {
                    try { cc.cluster.originPort = static_cast<std::uint16_t>(std::stoi(cc.cluster.originHost.substr(colon + 1))); } catch (...) {}
                    cc.cluster.originHost.erase(colon);
                }
            }
            cc.cluster.originTls        = cl.value("originTls", false);
            cc.cluster.originVerifyPeer = cl.value("originVerifyPeer", false);
            cc.cluster.trunks           = asU16(cl, "trunks", 1);
            if (cc.cluster.trunks < 1) cc.cluster.trunks = 1;
            cc.cluster.token            = cl.value("token", std::string{});
            cc.cluster.idleTimeoutSeconds = asU16(cl, "idleTimeoutSeconds", 90);
        }

        std::string chain = j.value("chain", std::string{"bitcoin"});
        if (chain == "bch" || chain == "bitcoincash" || chain == "BCH") {
            cc.chain = ChainKind::BitcoinCash;
        } else if (chain == "bc2" || chain == "btc2" || chain == "bitcoinii" || chain == "BC2" || chain == "BTC2") {
            cc.chain = ChainKind::BitcoinII;
        } else if (chain == "dgb" || chain == "digibyte" || chain == "DGB") {
            cc.chain = ChainKind::DigiByte;
        } else if (chain == "xec" || chain == "ecash" || chain == "XEC") {
            cc.chain = ChainKind::eCash;
        } else if (chain == "zec" || chain == "zcash" || chain == "ZEC") {
            cc.chain = ChainKind::Zcash;
        } else if (chain == "ltc" || chain == "litecoin" || chain == "LTC") {
            cc.chain = ChainKind::Litecoin;
        } else if (chain == "doge" || chain == "dogecoin" || chain == "DOGE") {
            cc.chain = ChainKind::Dogecoin;
        } else if (chain == "bch2" || chain == "bitcoincashii" || chain == "BCH2") {
            cc.chain = ChainKind::BitcoinCashII;
        } else {
            cc.chain = ChainKind::Bitcoin;
        }
        return cc;
    }
};

} // namespace mkpool
