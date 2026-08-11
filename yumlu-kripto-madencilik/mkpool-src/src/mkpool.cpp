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
// File:        mkpool.cpp
// Description: Program entry point: configuration load, service wiring and signal handling.
// Created:     2025-03-30
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"

#include "blacklist.hpp"
#include "config.hpp"
#include "database.hpp"
#include "db_worker.hpp"
#include "io_pool.hpp"
#include "logger.hpp"
#include "metrics.hpp"
#include "pool_manager.hpp"
#include "rate_limiter.hpp"

#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdlib>
#include <cxxopts.hpp>
#include <filesystem>
#include <functional>
#include <iostream>
#include <spdlog/spdlog.h>

namespace {

static mkpool::RateLimitConfig from_json(const mkpool::RateLimitJson& j) {
    mkpool::RateLimitConfig c;
    c.max_connections_per_ip      = j.maxConnectionsPerIP;
    c.accept_burst                = j.acceptBurst;
    c.accept_refill_per_sec       = j.acceptRefillPerSec;
    c.invalid_share_ban_threshold = j.invalidShareBanThreshold;
    c.invalid_share_window        = std::chrono::seconds(j.invalidShareWindowSec);
    c.ban_duration                = std::chrono::seconds(j.banDurationSec);
    return c;
}

} // anonymous

int main(int argc, char** argv) {
    cxxopts::Options opts("mkpool", "Modern Bitcoin/BCH mining pool");
    opts.add_options()
        ("c,config", "Config file", cxxopts::value<std::string>()->default_value("config.json"))
        ("h,help",   "Print usage");
    auto args = opts.parse(argc, argv);
    if (args.count("help")) { std::cout << opts.help() << '\n'; return 0; }
    const auto config_file = args["config"].as<std::string>();

    if (!std::filesystem::exists(config_file)) {
        std::cerr << "config file not found: " << config_file << '\n';
        return 1;
    }

    mkpool::Config cfg;
    try {
        auto loaded = mkpool::Config::fromJsonFile(config_file);
        if (!loaded) {
            std::cerr << "config file unreadable: " << config_file << '\n';
            return 1;
        }
        cfg = std::move(*loaded);
    } catch (const std::exception& e) {
        std::cerr << "config parse: " << e.what() << '\n';
        return 1;
    }

    // Derive a per-instance log name from the config filename so each coin/net
    // process writes its own rotating log file (e.g. "config-btc-mainnet.json"
    // -> "btc-mainnet" -> /var/log/mkpool/mkpool-btc-mainnet.log).
    std::string instance_name = std::filesystem::path(config_file).stem().string();
    if (const std::string p = "config-"; instance_name.rfind(p, 0) == 0) {
        instance_name.erase(0, p.size());
    }

    // Default the runtime control socket to a per-instance path so each coin/net
    // process gets its own socket. Explicit config (including "off") wins.
    if (cfg.global.controlSocket.empty()) {
        cfg.global.controlSocket = "/run/mkpool/" + instance_name + ".sock";
    }

    mkpool::Logger::init(cfg, instance_name);
    spdlog::info("mkpool starting; coins={} workers={} shards={}",
                 cfg.coins.size(),
                 cfg.global.ioThreads == 0
                     ? std::thread::hardware_concurrency()
                     : cfg.global.ioThreads,
                 cfg.global.sessionShards);

    std::vector<mkpool::CoinConfig> coin_vec;
    coin_vec.reserve(cfg.coins.size());
    for (auto& [_, cc] : cfg.coins) coin_vec.push_back(cc);

    // The scale-out relay roles (proxy / redirector / passthrough / node) are
    // stateless: they never record shares, blocks, presence or accounting, so
    // they need no database or blacklist and run fully standalone. Only the solo
    // pool role touches the DB (and it is the pool origin that keeps the accounting
    // for any clustered miners a passthrough/node forwards to it).
    const bool needs_db = (cfg.global.role == mkpool::PoolRole::Pool);

    // ----- Database (schema only here) -----
    if (needs_db) {
        try {
            mkpool::Database::init(cfg);
        } catch (const std::exception& e) {
            spdlog::critical("database init failed: {}", e.what());
            return 2;
        }

        // ----- Worker/address blacklist (load now; refresh on a timer + SIGHUP) -----
        mkpool::Blacklist::instance().reload();

        // ----- Async DB writer -----
        mkpool::DbWorkerConfig db_cfg;
        const std::string& active_coin = cfg.activeCoin;
        db_cfg.coin = active_coin;  // primary coin for block tagging / effort ownership
        if (active_coin == "LTC" || active_coin == "DOGE") {
            db_cfg.share_multiplier = 65536.0;
        } else if (active_coin == "ZEC") {
            db_cfg.share_multiplier = 8192.0;
        } else {
            db_cfg.share_multiplier = 4294967296.0;
        }
        mkpool::set_db_worker(std::make_unique<mkpool::DbWorker>(db_cfg));
        mkpool::db_worker().start();
    } else {
        spdlog::info("role '{}' is stateless; skipping database + blacklist",
                     mkpool::role_to_string(cfg.global.role));
    }

    // ----- Metrics -----
    mkpool::metrics::init(cfg.global.metricsListenAddress, cfg.global.metricsListenPort);

    // ----- IO pool -----
    mkpool::IoPool workers(cfg.global.ioThreads);

    // ----- Rate limiter -----
    auto rl = std::make_shared<mkpool::RateLimiter>(from_json(cfg.global.rateLimit));

    // ----- Pool manager (per-coin runtime) -----
    mkpool::PoolManager mgr(workers.main(), workers, cfg.global, coin_vec, rl);
    mgr.start();

    // Periodically refresh the in-memory blacklist from the DB (every 60s) so
    // table edits take effect without a restart, with zero per-connection cost.
    // Stateless relay roles have no DB/blacklist, so this loop is skipped.
    boost::asio::steady_timer blacklist_timer(workers.main());
    std::function<void(const boost::system::error_code&)> on_blacklist_tick;
    if (needs_db) {
        on_blacklist_tick = [&](const boost::system::error_code& ec) {
            if (ec) return; // cancelled on shutdown
            mkpool::Blacklist::instance().reload();
            blacklist_timer.expires_after(std::chrono::seconds(60));
            blacklist_timer.async_wait(on_blacklist_tick);
        };
        blacklist_timer.expires_after(std::chrono::seconds(60));
        blacklist_timer.async_wait(on_blacklist_tick);
    }

    boost::asio::signal_set signals(workers.main(), SIGINT, SIGTERM);
    std::function<void(const boost::system::error_code&, int)> on_signal;
    on_signal = [&](const boost::system::error_code& ec, int sig) {
        if (ec) return; // cancelled on shutdown
        // Signals are delivered on the main io thread, so this flag needs no
        // synchronisation. First signal starts a graceful drain (if enabled); a
        // second signal forces an immediate stop.
        static bool draining = false;
        if (cfg.global.restartDrainSeconds > 0 && !draining) {
            draining = true;
            spdlog::warn("signal {} received; graceful drain over {}s "
                         "(send the signal again to force an immediate stop)",
                         sig, cfg.global.restartDrainSeconds);
            mgr.beginGracefulShutdown(static_cast<int>(cfg.global.restartDrainSeconds),
                                      [&] { workers.stop(); });
            signals.async_wait(on_signal); // re-arm: a second signal forces stop
        } else {
            spdlog::info("signal {} received; shutting down", sig);
            mgr.stop();
            workers.stop();
        }
    };
    signals.async_wait(on_signal);

#ifdef SIGHUP
    // SIGHUP reloads TLS certificates in place (for unattended Let's Encrypt
    // renewals) without dropping connected miners. Re-arms after each delivery.
    boost::asio::signal_set hup(workers.main(), SIGHUP);
    std::function<void(const boost::system::error_code&, int)> on_hup;
    on_hup = [&](const boost::system::error_code& ec, int) {
        if (ec) return; // aborted on shutdown
        spdlog::info("SIGHUP received; reloading TLS certificates{}",
                     needs_db ? " + blacklist" : "");
        mgr.reloadTls();
        if (needs_db) mkpool::Blacklist::instance().reload();
        hup.async_wait(on_hup);
    };
    hup.async_wait(on_hup);
#endif

    workers.start();    // start worker threads
    workers.main().run();   // run on main thread
    workers.join();

    spdlog::info("workers stopped");
    if (needs_db) {
        spdlog::info("flushing DB writer");
        mkpool::db_worker().stop();
        mkpool::Database::shutdown();
    }
    mkpool::metrics::shutdown();
    mkpool::Logger::shutdown();
    return 0;
}
