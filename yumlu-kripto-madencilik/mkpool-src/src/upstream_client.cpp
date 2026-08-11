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
// File:        upstream_client.cpp
// Description: Outbound Stratum V1 client + multi-link relay state for the proxy role.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "upstream_client.hpp"

#include "utils.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace mkpool {

// ===========================================================================
// UpstreamConn - transport only
// ===========================================================================

UpstreamConn::UpstreamConn(boost::asio::io_context& io,
                           std::string host, std::string port,
                           bool tls,
                           std::shared_ptr<boost::asio::ssl::context> tls_ctx,
                           LineHandler on_line, OpenHandler on_open, CloseHandler on_close)
    : io_(io),
      strand_(boost::asio::make_strand(io.get_executor())),
      resolver_(io),
      host_(std::move(host)),
      port_(std::move(port)),
      tls_ctx_(std::move(tls_ctx)),
      is_tls_(tls),
      socket_(io, is_tls_ ? tls_ctx_.get() : nullptr),
      on_line_(std::move(on_line)),
      on_open_(std::move(on_open)),
      on_close_(std::move(on_close)) {}

void UpstreamConn::start() {
    auto self = shared_from_this();
    resolver_.async_resolve(host_, port_,
        boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code& ec,
                   boost::asio::ip::tcp::resolver::results_type results) {
                if (ec) { self->close("resolve: " + ec.message()); return; }
                boost::asio::async_connect(self->socket_.lowest_layer(), results,
                    boost::asio::bind_executor(self->strand_,
                        [self](const boost::system::error_code& cec,
                               const boost::asio::ip::tcp::endpoint&) {
                            if (cec) { self->close("connect: " + cec.message()); return; }
                            boost::system::error_code oec;
                            self->socket_.lowest_layer().set_option(
                                boost::asio::ip::tcp::no_delay(true), oec);
                            if (self->is_tls_) {
                                self->socket_.async_handshake(
                                    boost::asio::ssl::stream_base::client,
                                    boost::asio::bind_executor(self->strand_,
                                        [self](const boost::system::error_code& hec) {
                                            if (hec) { self->close("tls handshake: " + hec.message()); return; }
                                            self->on_open_();
                                            self->do_read();
                                        }));
                            } else {
                                self->on_open_();
                                self->do_read();
                            }
                        }));
            }));
}

void UpstreamConn::send(std::string line) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, line = std::move(line)]() mutable {
        if (self->closed_.load()) return;
        self->wq_.emplace_back(std::move(line));
        if (!self->writing_) { self->writing_ = true; self->do_write(); }
    });
}

void UpstreamConn::do_write() {
    if (closed_.load() || wq_.empty()) { writing_ = false; return; }
    auto self = shared_from_this();
    auto& front = wq_.front();
    boost::asio::async_write(socket_, boost::asio::buffer(front.data(), front.size()),
        boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code& ec, std::size_t) {
                if (ec) { self->writing_ = false; self->close("write: " + ec.message()); return; }
                if (self->closed_.load() || self->wq_.empty()) { self->writing_ = false; return; }
                self->wq_.pop_front();
                if (self->wq_.empty()) self->writing_ = false;
                else self->do_write();
            }));
}

void UpstreamConn::do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(read_buf_.data(), read_buf_.size()),
        boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code& ec, std::size_t n) {
                self->on_read(ec, n);
            }));
}

void UpstreamConn::on_read(const boost::system::error_code& ec, std::size_t n) {
    if (ec) { close("read: " + ec.message()); return; }
    buffer_.append(read_buf_.data(), n);
    if (buffer_.size() > (4u << 20)) { close("oversize upstream buffer"); return; }
    while (true) {
        auto pos = buffer_.find('\n');
        if (pos == std::string::npos) break;
        std::string_view line(buffer_.data(), pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty() && on_line_) on_line_(line);
        buffer_.erase(0, pos + 1);
        if (closed_.load()) return;
    }
    if (!closed_.load()) do_read();
}

void UpstreamConn::close(const std::string& reason) {
    if (closed_.exchange(true)) return;
    boost::system::error_code ec;
    socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.lowest_layer().close(ec);
    if (on_close_) on_close_(reason);
}

// ===========================================================================
// UpstreamClient - multi-link coordinator
// ===========================================================================

namespace {
std::string json_escape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) { if (c == '"' || c == '\\') o.push_back('\\'); o.push_back(c); }
    return o;
}
} // anonymous

UpstreamClient::UpstreamClient(boost::asio::io_context& io, CoinConfig coin)
    : io_(io),
      strand_(boost::asio::make_strand(io.get_executor())),
      coin_(std::move(coin)),
      idle_timer_(io) {
    active_active_ = (coin_.upstream.mode == "activeactive" || coin_.upstream.mode == "active-active");
    auto eps = coin_.upstream.endpoints;
    std::stable_sort(eps.begin(), eps.end(),
                     [](const UpstreamEndpoint& a, const UpstreamEndpoint& b) { return a.priority < b.priority; });
    int idx = 0;
    for (auto& e : eps) {
        auto l = std::make_unique<Link>(io_);
        l->ep = e;
        l->index = idx++;
        links_.push_back(std::move(l));
    }
}

void UpstreamClient::start() {
    if (links_.empty()) {
        spdlog::error("[Upstream {}] no upstream endpoints configured; proxy idle", coin_.name);
        return;
    }
    running_.store(true);
    spdlog::info("[Upstream {}] mode={} with {} link(s)", coin_.name,
                 active_active_ ? "active-active" : "failover", links_.size());
    boost::asio::post(strand_, [self = shared_from_this()] {
        for (std::size_t i = 0; i < self->links_.size(); ++i) self->connect_link(static_cast<int>(i));
        self->arm_idle_watchdog();
    });
}

void UpstreamClient::stop() {
    running_.store(false);
    boost::asio::post(strand_, [self = shared_from_this()] {
        self->idle_timer_.cancel();
        for (auto& l : self->links_) {
            l->reconnect_timer.cancel();
            if (l->conn) l->conn->close("shutdown");
            l->conn.reset();
        }
    });
}

void UpstreamClient::connect_link(int i) {
    if (!running_.load()) return;
    Link& L = *links_[i];
    const auto& ep = L.ep;
    {
        std::lock_guard<std::mutex> lk(mu_);
        L.connected = L.authorized = L.subscribed = false;
    }
    L.id_configure = L.id_subscribe = L.id_authorize = 0;
    L.pending.clear();
    L.last_rx = std::chrono::steady_clock::now();

    std::shared_ptr<boost::asio::ssl::context> ctx;
    if (ep.tls) {
        ctx = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
        ctx->set_options(boost::asio::ssl::context::default_workarounds |
                         boost::asio::ssl::context::no_sslv2 | boost::asio::ssl::context::no_sslv3);
        if (ep.verifyPeer) { ctx->set_default_verify_paths(); ctx->set_verify_mode(boost::asio::ssl::verify_peer); }
        else               { ctx->set_verify_mode(boost::asio::ssl::verify_none); }
    }
    spdlog::info("[Upstream {} L{}] connecting to {}:{}{}", coin_.name, i, ep.host, ep.port, ep.tls ? " tls" : "");

    auto weak = weak_from_this();
    const std::uint64_t gen = ++L.conn_gen;
    L.conn = std::make_shared<UpstreamConn>(
        io_, ep.host, std::to_string(ep.port), ep.tls, ctx,
        [weak, i, gen](std::string_view line) {
            auto s = weak.lock(); if (!s) return;
            boost::asio::post(s->strand_, [s, i, gen, l = std::string(line)] {
                if (s->links_[i]->conn_gen == gen) s->on_line(i, l);
            });
        },
        [weak, i, gen] {
            auto s = weak.lock(); if (!s) return;
            boost::asio::post(s->strand_, [s, i, gen] { if (s->links_[i]->conn_gen == gen) s->on_conn_open(i); });
        },
        [weak, i, gen](const std::string& reason) {
            auto s = weak.lock(); if (!s) return;
            boost::asio::post(s->strand_, [s, i, gen, reason] { if (s->links_[i]->conn_gen == gen) s->on_conn_close(i, reason); });
        });
    L.conn->start();
}

void UpstreamClient::schedule_reconnect(int i) {
    if (!running_.load()) return;
    Link& L = *links_[i];
    const auto& uc = coin_.upstream;
    int lo = uc.reconnectMinSeconds > 0 ? uc.reconnectMinSeconds : 2;
    int hi = uc.reconnectMaxSeconds > 0 ? uc.reconnectMaxSeconds : 30;
    int delay = std::min(hi, lo << std::min(L.reconnect_backoff, 5));
    if (delay < lo) delay = lo;
    L.reconnect_backoff = std::min(L.reconnect_backoff + 1, 6);
    spdlog::info("[Upstream {} L{}] reconnecting in {}s", coin_.name, i, delay);
    L.reconnect_timer.expires_after(std::chrono::seconds(delay));
    L.reconnect_timer.async_wait(boost::asio::bind_executor(strand_,
        [self = shared_from_this(), i](const boost::system::error_code& ec) {
            if (ec || !self->running_.load()) return;
            self->connect_link(i);
        }));
}

void UpstreamClient::arm_idle_watchdog() {
    if (!running_.load()) return;
    idle_timer_.expires_after(std::chrono::seconds(15));
    idle_timer_.async_wait(boost::asio::bind_executor(strand_,
        [self = shared_from_this()](const boost::system::error_code& ec) {
            if (ec || !self->running_.load()) return;
            const int idle_to = self->coin_.upstream.idleTimeoutSeconds > 0
                                    ? self->coin_.upstream.idleTimeoutSeconds : 90;
            const auto now = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < self->links_.size(); ++i) {
                Link& L = *self->links_[i];
                bool conn; { std::lock_guard<std::mutex> lk(self->mu_); conn = L.connected; }
                if (conn && L.conn &&
                    std::chrono::duration_cast<std::chrono::seconds>(now - L.last_rx).count() > idle_to) {
                    spdlog::warn("[Upstream {} L{}] idle > {}s; dropping link", self->coin_.name, i, idle_to);
                    L.conn->close("idle timeout");
                }
                for (auto it = L.pending.begin(); it != L.pending.end();) {
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.ts).count() > 30) {
                        if (it->second.cb) it->second.cb(false, "timeout");
                        it = L.pending.erase(it);
                    } else ++it;
                }
            }
            self->arm_idle_watchdog();
        }));
}

void UpstreamClient::on_conn_open(int i) {
    spdlog::info("[Upstream {} L{}] TCP established; negotiating", coin_.name, i);
    { std::lock_guard<std::mutex> lk(mu_); links_[i]->connected = true; }
    links_[i]->last_rx = std::chrono::steady_clock::now();
    send_configure(i);
    send_subscribe(i);
    send_authorize(i);
}

void UpstreamClient::on_conn_close(int i, const std::string& reason) {
    spdlog::warn("[Upstream {} L{}] link down: {}", coin_.name, i, reason);
    { std::lock_guard<std::mutex> lk(mu_); Link& L = *links_[i]; L.connected = L.authorized = L.subscribed = false; }
    links_[i]->stat_reconnects.fetch_add(1, std::memory_order_relaxed);
    migrate_link(i);        // move this link's miners to a warm standby (if any)
    schedule_reconnect(i);  // and bring this endpoint back up as a standby
}

void UpstreamClient::send_configure(int i) {
    if (!coin_.enableVersionRolling) return;
    Link& L = *links_[i];
    L.id_configure = L.next_id++;
    std::string mask = fmt::format("{:08x}", coin_.versionRollingMask);
    if (L.conn) L.conn->send(fmt::format(
        R"({{"id":{},"method":"mining.configure","params":[["version-rolling"],{{"version-rolling.mask":"{}"}}]}})" "\n",
        L.id_configure, mask));
}

void UpstreamClient::send_subscribe(int i) {
    Link& L = *links_[i];
    L.id_subscribe = L.next_id++;
    if (L.conn) L.conn->send(fmt::format(
        R"({{"id":{},"method":"mining.subscribe","params":["mkpool-proxy/1.0"]}})" "\n", L.id_subscribe));
}

void UpstreamClient::send_authorize(int i) {
    Link& L = *links_[i];
    L.id_authorize = L.next_id++;
    if (L.conn) L.conn->send(fmt::format(
        R"({{"id":{},"method":"mining.authorize","params":["{}","{}"]}})" "\n",
        L.id_authorize, json_escape(L.ep.user), json_escape(L.ep.pass)));
}

void UpstreamClient::on_line(int i, std::string_view line) {
    links_[i]->last_rx = std::chrono::steady_clock::now();
    nlohmann::json msg;
    try { msg = nlohmann::json::parse(line); }
    catch (...) { spdlog::warn("[Upstream {} L{}] parse failed", coin_.name, i); return; }
    if (!msg.is_object()) return;
    try {
        if (msg.contains("method") && msg["method"].is_string())
            handle_method(i, msg["method"].get<std::string>(), msg);
        else if (msg.contains("id"))
            handle_result(i, msg);
    } catch (const std::exception& e) {
        spdlog::warn("[Upstream {} L{}] handler error: {}", coin_.name, i, e.what());
    }
}

void UpstreamClient::handle_result(int i, const nlohmann::json& msg) {
    Link& L = *links_[i];
    std::uint64_t id = 0;
    if (msg["id"].is_number_unsigned())     id = msg["id"].get<std::uint64_t>();
    else if (msg["id"].is_number_integer()) id = static_cast<std::uint64_t>(msg["id"].get<std::int64_t>());
    else return;
    const bool err_null = !msg.contains("error") || msg["error"].is_null();

    if (id == L.id_configure && L.id_configure != 0) {
        if (msg.contains("result") && msg["result"].is_object()) {
            const auto& r = msg["result"];
            std::uint32_t m = 0;
            if (r.contains("version-rolling") && r["version-rolling"].is_boolean() && r["version-rolling"].get<bool>() &&
                r.contains("version-rolling.mask") && r["version-rolling.mask"].is_string()) {
                try { m = static_cast<std::uint32_t>(std::stoul(r["version-rolling.mask"].get<std::string>(), nullptr, 16)); } catch (...) {}
            }
            std::lock_guard<std::mutex> lk(mu_); L.up_version_mask = m;
        }
        return;
    }
    if (id == L.id_subscribe && L.id_subscribe != 0) {
        if (!msg.contains("result") || !msg["result"].is_array() || msg["result"].size() < 3) {
            spdlog::error("[Upstream {} L{}] bad subscribe result; dropping", coin_.name, i);
            if (L.conn) L.conn->close("bad subscribe result");
            return;
        }
        const auto& r = msg["result"];
        std::string en1 = r[1].is_string() ? r[1].get<std::string>() : std::string{};
        std::size_t en2 = 0;
        if (r[2].is_number_unsigned())     en2 = r[2].get<std::size_t>();
        else if (r[2].is_number_integer()) en2 = static_cast<std::size_t>(r[2].get<long long>());
        set_upstream_extranonce(i, en1, en2, /*reconnect=*/false);
        { std::lock_guard<std::mutex> lk(mu_); L.subscribed = true; }
        spdlog::info("[Upstream {} L{}] subscribed en1={} en2_size={}", coin_.name, i, en1, en2);
        return;
    }
    if (id == L.id_authorize && L.id_authorize != 0) {
        const bool ok = msg.contains("result") && msg["result"].is_boolean() && msg["result"].get<bool>();
        if (!ok) {
            spdlog::error("[Upstream {} L{}] authorize REJECTED (check user/pass)", coin_.name, i);
            if (L.conn) L.conn->close("authorize rejected");
            return;
        }
        { std::lock_guard<std::mutex> lk(mu_); L.authorized = true; }
        L.reconnect_backoff = 0;
        spdlog::info("[Upstream {} L{}] authorized; link READY", coin_.name, i);
        notify_waiters();   // pending sessions can now attach
        return;
    }
    auto it = L.pending.find(id);
    if (it != L.pending.end()) {
        const bool accepted = err_null && msg.contains("result") && msg["result"].is_boolean() && msg["result"].get<bool>();
        if (accepted) L.stat_accepted.fetch_add(1, std::memory_order_relaxed);
        else          L.stat_rejected.fetch_add(1, std::memory_order_relaxed);
        std::string err;
        if (!accepted && msg.contains("error") && !msg["error"].is_null()) err = msg["error"].dump();
        if (it->second.cb) it->second.cb(accepted, err);
        L.pending.erase(it);
    }
}

void UpstreamClient::handle_method(int i, const std::string& method, const nlohmann::json& msg) {
    const auto* params = (msg.contains("params") && msg["params"].is_array()) ? &msg["params"] : nullptr;
    Link& L = *links_[i];
    if (method == "mining.set_difficulty") {
        if (params && !params->empty() && (*params)[0].is_number()) set_upstream_diff(i, (*params)[0].get<double>());
    } else if (method == "mining.notify") {
        if (params) build_and_broadcast_notify(i, *params);
    } else if (method == "mining.set_extranonce") {
        if (params && params->size() >= 2 && (*params)[0].is_string()) {
            std::string en1 = (*params)[0].get<std::string>();
            std::size_t en2 = (*params)[1].is_number() ? (*params)[1].get<std::size_t>() : L.up_en2_size;
            set_upstream_extranonce(i, en1, en2, /*reconnect=*/true);
        }
    } else if (method == "mining.set_version_mask") {
        if (params && !params->empty() && (*params)[0].is_string()) {
            try { std::uint32_t m = static_cast<std::uint32_t>(std::stoul((*params)[0].get<std::string>(), nullptr, 16));
                  std::lock_guard<std::mutex> lk(mu_); L.up_version_mask = m; } catch (...) {}
        }
    } else if (method == "client.reconnect") {
        spdlog::info("[Upstream {} L{}] upstream requested client.reconnect", coin_.name, i);
        if (L.conn) L.conn->close("upstream client.reconnect");
    }
}

void UpstreamClient::set_upstream_diff(int i, double diff) {
    if (diff <= 0.0) return;
    { std::lock_guard<std::mutex> lk(mu_); links_[i]->up_diff = diff; }
    spdlog::info("[Upstream {} L{}] set_difficulty {}", coin_.name, i, diff);
    for (auto& s : link_sinks(i)) s->on_upstream_diff(diff);
}

void UpstreamClient::set_upstream_extranonce(int i, std::string en1, std::size_t en2_size, bool reconnect) {
    Link& L = *links_[i];
    std::uint8_t want = coin_.upstream.nonceBytes ? coin_.upstream.nonceBytes : 2;
    std::uint8_t eff = want;
    if (en2_size <= 1) eff = 0;
    else if (want >= en2_size) eff = static_cast<std::uint8_t>(en2_size - 1);

    std::vector<std::tuple<std::shared_ptr<ProxyWorkSink>, std::string, std::size_t>> reissue;
    {
        std::lock_guard<std::mutex> lk(mu_);
        L.up_en1 = std::move(en1);
        L.up_en2_size = en2_size;
        L.nonce_bytes_eff = eff;
        L.slot_span = (eff == 0) ? 1u : (1u << (8u * eff));
        if (reconnect) {
            const std::size_t down_en2 = (en2_size > eff) ? (en2_size - eff) : 0;
            for (auto& [sid, sub] : L.subscribers) {
                auto sink = sub.sink.lock();
                if (!sink) continue;
                reissue.emplace_back(sink, L.up_en1 + sub.slot_hex, down_en2);
            }
        }
    }
    for (auto& [sink, e1, e2] : reissue) sink->on_upstream_extranonce(e1, e2);
}

void UpstreamClient::build_and_broadcast_notify(int i, const nlohmann::json& params) {
    if (params.size() < 9) { spdlog::warn("[Upstream {} L{}] short notify", coin_.name, i); return; }
    auto job = std::make_shared<ProxyNotify>();
    try {
        job->job_id           = params[0].get<std::string>();
        job->prevhash_stratum = params[1].get<std::string>();
        job->coinbase1        = params[2].get<std::string>();
        job->coinbase2        = params[3].get<std::string>();
        if (params[4].is_array())
            for (const auto& b : params[4]) if (b.is_string()) job->merkle_branch.push_back(b.get<std::string>());
        job->bits  = params[6].get<std::string>();
        job->ntime = params[7].get<std::string>();
        job->clean = params[8].is_boolean() ? params[8].get<bool>() : false;
        job->version = static_cast<std::uint32_t>(std::stoul(params[5].get<std::string>(), nullptr, 16));
        job->mintime = static_cast<std::uint32_t>(std::stoul(job->ntime, nullptr, 16));
        job->net_target_be = utils::target_from_bits_hex(job->bits);
    } catch (const std::exception& e) {
        spdlog::warn("[Upstream {} L{}] notify decode failed: {}", coin_.name, i, e.what());
        return;
    }
    nlohmann::json note = {{"id", nullptr}, {"method", "mining.notify"}, {"params", params}};
    job->notify_line = note.dump();
    job->notify_line.push_back('\n');
    {
        std::lock_guard<std::mutex> lk(mu_);
        job->seq = ++links_[i]->job_seq;
        links_[i]->cur_job = job;
    }
    links_[i]->stat_jobs.fetch_add(1, std::memory_order_relaxed);
    spdlog::info("[Upstream {} L{}] notify job={} clean={} branch_n={}", coin_.name, i, job->job_id, job->clean, job->merkle_branch.size());
    for (auto& s : link_sinks(i)) s->on_upstream_job(job);
}

std::vector<std::shared_ptr<ProxyWorkSink>> UpstreamClient::link_sinks(int i) const {
    std::vector<std::shared_ptr<ProxyWorkSink>> out;
    std::lock_guard<std::mutex> lk(mu_);
    const Link& L = *links_[i];
    out.reserve(L.subscribers.size());
    for (auto& [sid, sub] : L.subscribers) if (auto s = sub.sink.lock()) out.push_back(std::move(s));
    return out;
}

// ---- assignment + allocation (mu_ held) ----

bool UpstreamClient::link_ready_locked(const Link& l) const {
    return l.connected && l.authorized && l.subscribed && !l.up_en1.empty()
           && l.slot_span > 0 && l.nonce_bytes_eff > 0 && l.subscribers.size() < l.slot_span;
}

int UpstreamClient::pick_link_locked() const {
    // Choose an assignment target among ready links.
    int best = -1;
    if (active_active_) {
        double best_load = std::numeric_limits<double>::max();
        for (auto& lp : links_) {
            if (!link_ready_locked(*lp)) continue;
            double load = static_cast<double>(lp->subscribers.size()) / std::max<std::uint32_t>(1, lp->ep.weight);
            if (load < best_load) { best_load = load; best = lp->index; }
        }
        return best;
    }
    // failover: sticky primary if it is still ready, else lowest-priority ready.
    if (primary_ >= 0 && primary_ < static_cast<int>(links_.size()) && link_ready_locked(*links_[primary_]))
        return primary_;
    for (auto& lp : links_) {  // links_ is priority-sorted
        if (link_ready_locked(*lp)) { best = lp->index; break; }
    }
    return best;
}

ProxySlot UpstreamClient::allocate_on_locked(Link& L, const std::weak_ptr<ProxyWorkSink>& sink) {
    ProxySlot slot;
    if (!link_ready_locked(L)) return slot;
    const std::uint8_t nb = L.nonce_bytes_eff;
    for (std::uint32_t tries = 0; tries < L.slot_span; ++tries) {
        std::uint32_t cand = L.next_slot;
        L.next_slot = (L.next_slot + 1) % L.slot_span;
        if (L.subscribers.find(cand) != L.subscribers.end()) continue;
        std::string slot_hex = fmt::format("{:0{}x}", cand, nb * 2);
        L.subscribers.emplace(cand, Subscriber{sink, slot_hex});
        slot.ok = true; slot.link_id = L.index; slot.slot_id = cand; slot.slot_hex = slot_hex;
        slot.en1_hex = L.up_en1 + slot_hex;
        slot.en2_size = (L.up_en2_size > nb) ? (L.up_en2_size - nb) : 0;
        return slot;
    }
    return slot;
}

ProxySlot UpstreamClient::attach(const std::weak_ptr<ProxyWorkSink>& sink) {
    std::lock_guard<std::mutex> lk(mu_);
    int i = pick_link_locked();
    if (i < 0) return ProxySlot{};
    if (!active_active_) primary_ = i;   // sticky
    return allocate_on_locked(*links_[i], sink);
}

void UpstreamClient::detach(int link_id, std::uint32_t slot_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (link_id < 0 || link_id >= static_cast<int>(links_.size())) return;
    links_[link_id]->subscribers.erase(slot_id);
}

void UpstreamClient::register_waiter(const std::weak_ptr<ProxyWorkSink>& sink) {
    std::lock_guard<std::mutex> lk(mu_);
    waiters_.push_back(sink);
}

void UpstreamClient::notify_waiters() {
    std::vector<std::shared_ptr<ProxyWorkSink>> w;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& ws : waiters_) if (auto s = ws.lock()) w.push_back(std::move(s));
        waiters_.clear();
    }
    for (auto& s : w) s->on_upstream_state(true);
}

void UpstreamClient::migrate_link(int i) {
    struct Reassign { std::shared_ptr<ProxyWorkSink> sink; ProxySlot slot; ProxyNotifyPtr job; double diff; };
    std::vector<Reassign> reassigns;
    std::vector<std::shared_ptr<ProxyWorkSink>> stranded;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Link& L = *links_[i];
        if (primary_ == i) primary_ = -1;
        // failover: everyone moves to a single new primary; active-active spreads.
        int failover_target = -1;
        if (!active_active_) {
            for (auto& lp : links_) { if (lp->index != i && link_ready_locked(*lp)) { failover_target = lp->index; break; } }
            if (failover_target >= 0) primary_ = failover_target;
        }
        std::vector<std::uint32_t> erase_ids;
        for (auto& [sid, sub] : L.subscribers) {
            auto sink = sub.sink.lock();
            if (!sink) { erase_ids.push_back(sid); continue; }
            int t = failover_target;
            if (active_active_) {
                double best_load = std::numeric_limits<double>::max(); t = -1;
                for (auto& lp : links_) {
                    if (lp->index == i || !link_ready_locked(*lp)) continue;
                    double load = static_cast<double>(lp->subscribers.size()) / std::max<std::uint32_t>(1, lp->ep.weight);
                    if (load < best_load) { best_load = load; t = lp->index; }
                }
            }
            if (t < 0) { stranded.push_back(sink); continue; } // no standby: keep bound, recover on reconnect
            ProxySlot ns = allocate_on_locked(*links_[t], sub.sink);
            if (!ns.ok) { stranded.push_back(sink); continue; }
            reassigns.push_back({sink, ns, links_[t]->cur_job, links_[t]->up_diff});
            erase_ids.push_back(sid);
        }
        for (auto sid : erase_ids) L.subscribers.erase(sid);
    }
    if (!reassigns.empty())
        spdlog::warn("[Upstream {} L{}] migrating {} downstream miner(s) to a standby link",
                     coin_.name, i, reassigns.size());
    for (auto& r : reassigns) r.sink->on_rebind(r.slot, r.job, r.diff);
    for (auto& s : stranded)  s->on_upstream_state(false);
}

// ---- session-facing queries ----

ProxyNotifyPtr UpstreamClient::current_job(int link_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (link_id < 0 || link_id >= static_cast<int>(links_.size())) return nullptr;
    return links_[link_id]->cur_job;
}

double UpstreamClient::current_diff(int link_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (link_id < 0 || link_id >= static_cast<int>(links_.size())) return 1.0;
    return links_[link_id]->up_diff;
}

bool UpstreamClient::any_ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& lp : links_) if (link_ready_locked(*lp)) return true;
    return false;
}

void UpstreamClient::submit(int link_id, const UpstreamSubmit& s, SubmitCallback cb) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, link_id, s, cb = std::move(cb)]() mutable {
        if (link_id < 0 || link_id >= static_cast<int>(self->links_.size())) { if (cb) cb(false, "bad link"); return; }
        Link& L = *self->links_[link_id];
        L.stat_forwarded.fetch_add(1, std::memory_order_relaxed);
        if (!L.conn || L.conn->closed()) {
            if (cb) cb(false, "upstream down");
            L.stat_rejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint64_t id = L.next_id++;
        std::string vpart;
        if (s.version_bits) vpart = fmt::format(R"(,"{:08x}")", *s.version_bits);
        std::string line = fmt::format(
            R"({{"id":{},"method":"mining.submit","params":["{}","{}","{}","{}","{}"{}]}})" "\n",
            id, json_escape(L.ep.user), s.job_id, s.en2_full_hex, s.ntime_hex, s.nonce_hex, vpart);
        L.pending.emplace(id, PendingSubmit{std::move(cb), std::chrono::steady_clock::now()});
        L.conn->send(std::move(line));
    });
}

UpstreamStatus UpstreamClient::status() const {
    UpstreamStatus st;
    st.mode = active_active_ ? "active-active" : "failover";
    std::lock_guard<std::mutex> lk(mu_);
    st.primary = primary_;
    for (auto& lp : links_) {
        LinkStatus ls;
        ls.index = lp->index;
        ls.endpoint = fmt::format("{}:{}", lp->ep.host, lp->ep.port);
        ls.connected = lp->connected; ls.authorized = lp->authorized;
        ls.primary = (lp->index == primary_);
        ls.difficulty = lp->up_diff; ls.extranonce1 = lp->up_en1; ls.en2_size = lp->up_en2_size;
        ls.jobs = lp->stat_jobs.load(std::memory_order_relaxed);
        ls.shares_forwarded = lp->stat_forwarded.load(std::memory_order_relaxed);
        ls.shares_accepted = lp->stat_accepted.load(std::memory_order_relaxed);
        ls.shares_rejected = lp->stat_rejected.load(std::memory_order_relaxed);
        ls.reconnects = lp->stat_reconnects.load(std::memory_order_relaxed);
        ls.sessions = lp->subscribers.size();
        st.total_sessions += ls.sessions;
        st.links.push_back(std::move(ls));
    }
    return st;
}

} // namespace mkpool
