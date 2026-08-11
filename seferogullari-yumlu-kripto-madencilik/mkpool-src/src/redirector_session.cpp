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
// File:        redirector_session.cpp
// Description: Downstream session for the redirector role (client.reconnect steering).
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "redirector_session.hpp"

#include "stratum_protocol.hpp"
#include "utils.hpp"

#include <random>
#include <spdlog/spdlog.h>

namespace mkpool {

namespace {
std::string safe_id_json(const nlohmann::json& msg) {
    auto it = msg.find("id");
    if (it == msg.end() || it->is_null()) return "null";
    try {
        if (it->is_number_integer())  return fmt::format("{}", it->get<std::int64_t>());
        if (it->is_number_unsigned()) return fmt::format("{}", it->get<std::uint64_t>());
        if (it->is_string())          return "\"" + it->get<std::string>() + "\"";
    } catch (...) {}
    return "null";
}
} // anonymous

RedirectorPolicy::RedirectorPolicy(boost::asio::io_context& io, const RedirectConfig& cfg)
    : io_(io),
      strategy_(cfg.strategy),
      wait_(cfg.wait),
      health_enabled_(cfg.healthCheck),
      interval_s_(cfg.healthCheckIntervalSeconds > 0 ? cfg.healthCheckIntervalSeconds : 10),
      timeout_ms_(cfg.healthCheckTimeoutMs > 0 ? cfg.healthCheckTimeoutMs : 1000),
      timer_(io) {
    int idx = 0;
    for (const auto& t : cfg.targets) {
        auto tp = std::make_unique<Target>();
        tp->host = t.host; tp->port = t.port; tp->weight = t.weight ? t.weight : 1;
        std::uint32_t w = std::min<std::uint32_t>(tp->weight, 1000);  // cap wheel expansion
        for (std::uint32_t k = 0; k < w; ++k) wheel_.push_back(idx);
        targets_.push_back(std::move(tp));
        ++idx;
    }
}

RedirectorPolicy::Pick RedirectorPolicy::next() {
    if (targets_.empty()) return Pick{};
    auto healthy = [&](int i) { return !health_enabled_ || targets_[i]->up.load(std::memory_order_relaxed); };
    bool any_healthy = false;
    for (std::size_t i = 0; i < targets_.size(); ++i) if (healthy(static_cast<int>(i))) { any_healthy = true; break; }
    auto ok = [&](int i) { return any_healthy ? healthy(i) : true; };  // fail open if all down

    if (strategy_ == "latency" && health_enabled_) {
        int best = -1; double bl = 1e18;
        for (std::size_t i = 0; i < targets_.size(); ++i) {
            if (!ok(static_cast<int>(i))) continue;
            double l = targets_[i]->latency_ms.load(std::memory_order_relaxed);
            if (l < bl) { bl = l; best = static_cast<int>(i); }
        }
        if (best >= 0) return Pick{targets_[best]->host, targets_[best]->port};
    }
    if (strategy_ == "random") {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::vector<int> c;
        for (std::size_t i = 0; i < targets_.size(); ++i) if (ok(static_cast<int>(i))) c.push_back(static_cast<int>(i));
        if (!c.empty()) { int i = c[rng() % c.size()]; return Pick{targets_[i]->host, targets_[i]->port}; }
    }
    // round-robin (default): advance over the weighted wheel, skipping unhealthy.
    const std::size_t n = wheel_.size();
    for (std::size_t step = 0; step < n; ++step) {
        int i = wheel_[cursor_.fetch_add(1, std::memory_order_relaxed) % n];
        if (ok(i)) return Pick{targets_[i]->host, targets_[i]->port};
    }
    return Pick{targets_[0]->host, targets_[0]->port};
}

void RedirectorPolicy::start() {
    if (!health_enabled_ || targets_.empty()) return;
    running_.store(true);
    spdlog::info("[redirector] health probing enabled: {} backend(s), every {}s, timeout {}ms",
                 targets_.size(), interval_s_, timeout_ms_);
    boost::asio::post(io_, [self = shared_from_this()] { self->probe_all(); self->schedule_probe(); });
}

void RedirectorPolicy::stop() {
    running_.store(false);
    timer_.cancel();
}

void RedirectorPolicy::schedule_probe() {
    if (!running_.load()) return;
    timer_.expires_after(std::chrono::seconds(interval_s_));
    timer_.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (ec || !self->running_.load()) return;
        self->probe_all();
        self->schedule_probe();
    });
}

void RedirectorPolicy::probe_all() {
    using boost::asio::ip::tcp;
    for (auto& tp : targets_) {
        Target* t = tp.get();
        auto self = shared_from_this();
        auto resolver = std::make_shared<tcp::resolver>(io_);
        auto sock     = std::make_shared<tcp::socket>(io_);
        auto deadline = std::make_shared<boost::asio::steady_timer>(io_);
        auto started  = std::chrono::steady_clock::now();
        resolver->async_resolve(t->host, std::to_string(t->port),
            [self, t, resolver, sock, deadline, started](const boost::system::error_code& rec,
                                                         tcp::resolver::results_type results) {
                if (rec) { t->up.store(false, std::memory_order_relaxed); return; }
                deadline->expires_after(std::chrono::milliseconds(self->timeout_ms_));
                deadline->async_wait([sock](const boost::system::error_code& dec) {
                    if (!dec) { boost::system::error_code c; sock->close(c); }  // timeout: abort the connect
                });
                boost::asio::async_connect(*sock, results,
                    [t, sock, deadline, started](const boost::system::error_code& cec, const tcp::endpoint&) {
                        deadline->cancel();
                        if (cec) {
                            t->up.store(false, std::memory_order_relaxed);
                        } else {
                            double ms = std::chrono::duration<double, std::milli>(
                                            std::chrono::steady_clock::now() - started).count();
                            t->latency_ms.store(ms, std::memory_order_relaxed);
                            t->up.store(true, std::memory_order_relaxed);
                        }
                        boost::system::error_code c; sock->close(c);
                    });
            });
    }
}

std::vector<RedirectorPolicy::HealthRow> RedirectorPolicy::health() const {
    std::vector<HealthRow> out;
    out.reserve(targets_.size());
    for (auto& tp : targets_)
        out.push_back(HealthRow{tp->host, tp->port,
                                tp->up.load(std::memory_order_relaxed),
                                tp->latency_ms.load(std::memory_order_relaxed)});
    return out;
}

RedirectorSession::RedirectorSession(boost::asio::io_context& io,
                                     std::shared_ptr<RateLimiter> rl,
                                     RedirectorPolicyPtr policy,
                                     std::shared_ptr<boost::asio::ssl::context> tls_ctx)
    : RelaySession(io, std::move(rl), std::move(tls_ctx)),
      policy_(std::move(policy)),
      close_timer_(io) {
    session_id_ = utils::generate_session_id();
}

void RedirectorSession::on_line(std::string_view line) {
    nlohmann::json msg;
    try { msg = nlohmann::json::parse(line); }
    catch (...) { return; }
    if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string()) return;

    const std::string method = msg["method"].get<std::string>();
    const auto id_json = safe_id_json(msg);
    using namespace stratum;

    if (method == kMethodConfigure) {
        // Minimal BIP310 reply; nothing to negotiate at a redirector.
        send_line(fmt::format(R"({{"id":{},"result":{{}},"error":null}})" "\n", id_json));
        return;
    }
    if (method == kMethodSubscribe) {
        // Give the miner a well-formed subscribe result so strict firmware treats
        // the session as live, then immediately steer it elsewhere.
        send_line(fmt::format(
            R"({{"id":{},"result":[[["mining.set_difficulty","{}"],["mining.notify","{}"]],"00000000",4],"error":null}})" "\n",
            id_json, session_id_, session_id_));
        redirect_now();
        return;
    }
    // authorize / submit / anything else after we've decided to redirect: ignore.
}

void RedirectorSession::redirect_now() {
    if (redirected_) return;
    redirected_ = true;
    if (!policy_ || policy_->empty()) {
        spdlog::warn("[redirector {}] no backends configured; closing", client_ip_);
        close("no backends");
        return;
    }
    auto pick = policy_->next();
    const int wait = policy_->wait();
    spdlog::info("[redirector {}] -> client.reconnect {}:{} wait={}",
                 client_ip_, pick.host, pick.port, wait);
    send_line(fmt::format(
        R"({{"id":null,"method":"client.reconnect","params":["{}",{},{}]}})" "\n",
        pick.host, pick.port, wait));

    // Let the reconnect flush, then drop the connection. The miner should have
    // reconnected to the backend by then; keeping it open serves no purpose.
    auto self = shared_from_this();
    close_timer_.expires_after(std::chrono::seconds(5));
    close_timer_.async_wait(boost::asio::bind_executor(strand(),
        [self](const boost::system::error_code& ec) {
            if (ec) return;
            self->shutdown();   // public; posts close() to the strand
        }));
}

} // namespace mkpool
