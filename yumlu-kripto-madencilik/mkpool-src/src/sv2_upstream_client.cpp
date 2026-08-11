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
// File:        sv2_upstream_client.cpp
// Description: Outbound Stratum V2 client (V1 downstream -> SV2 upstream translation).
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "sv2_upstream_client.hpp"

#include "utils.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace mkpool {

namespace {

// header-LE prev_hash <-> stratum-notify prev_hash: reverse the 4 bytes within
// each of the 8 words. This map is its own inverse, so it converts either way.
std::string swap_words_hex(std::string_view p) {
    if (p.size() != 64) return std::string(p);
    std::string out; out.resize(64);
    for (std::size_t w = 0; w < 8; ++w)
        for (std::size_t b = 0; b < 4; ++b) {
            out[w * 8 + b * 2]     = p[w * 8 + (3 - b) * 2];
            out[w * 8 + b * 2 + 1] = p[w * 8 + (3 - b) * 2 + 1];
        }
    return out;
}

double diff_from_target_le(const std::array<std::uint8_t, 32>& t) {
    long double tv = 0.0L, base = 1.0L;
    for (int i = 0; i < 32; ++i) { tv += (long double)t[i] * base; base *= 256.0L; }
    long double maxt = (long double)0xFFFF * std::pow(2.0L, 208.0L);
    return tv > 0.0L ? (double)(maxt / tv) : 1.0;
}

bool parse_hex_u32(const std::string& h, std::uint32_t& out) {
    try { out = static_cast<std::uint32_t>(std::stoul(h, nullptr, 16)); return true; }
    catch (...) { return false; }
}

} // anonymous

Sv2UpstreamClient::Sv2UpstreamClient(boost::asio::io_context& io, CoinConfig coin)
    : io_(io),
      strand_(boost::asio::make_strand(io.get_executor())),
      coin_(std::move(coin)),
      socket_(io),
      reconnect_timer_(io) {
    if (!coin_.upstream.endpoints.empty()) {
        host_ = coin_.upstream.endpoints.front().host;
        port_ = std::to_string(coin_.upstream.endpoints.front().port);
    }
    nonce_bytes_ = coin_.upstream.nonceBytes ? coin_.upstream.nonceBytes : 2;
}

void Sv2UpstreamClient::start() {
    if (host_.empty()) { spdlog::error("[Sv2Upstream {}] no upstream endpoint", coin_.name); return; }
    running_.store(true);
    spdlog::info("[Sv2Upstream {}] SV2 upstream {}:{}", coin_.name, host_, port_);
    boost::asio::post(strand_, [self = shared_from_this()] { self->connect(); });
}

void Sv2UpstreamClient::stop() {
    running_.store(false);
    boost::asio::post(strand_, [self = shared_from_this()] { self->teardown("stop"); });
}

// ---------------------------------------------------------------------------
// transport
// ---------------------------------------------------------------------------
void Sv2UpstreamClient::connect() {
    if (!running_.load()) return;
    { std::lock_guard lk(mu_); connected_ = setup_ok_ = channel_ok_ = false; }
    have_header_ = false; in_.clear();
    auto self = shared_from_this();
    auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(io_);
    resolver->async_resolve(host_, port_, boost::asio::bind_executor(strand_,
        [self, resolver](const boost::system::error_code& ec, auto results) {
            if (ec) { self->teardown("resolve: " + ec.message()); return; }
            boost::asio::async_connect(self->socket_, results, boost::asio::bind_executor(self->strand_,
                [self](const boost::system::error_code& cec, const auto&) {
                    if (cec) { self->teardown("connect: " + cec.message()); return; }
                    boost::system::error_code oec;
                    self->socket_.set_option(boost::asio::ip::tcp::no_delay(true), oec);
                    { std::lock_guard lk(self->mu_); self->connected_ = true; }
                    self->reconnect_backoff_ = 0;
                    spdlog::info("[Sv2Upstream {}] connected; sending SetupConnection", self->coin_.name);
                    // SetupConnection: Mining protocol, versions 2..2, no special flags.
                    sv2::Writer w;
                    w.write_u8(0); w.write_u16(2); w.write_u16(2); w.write_u32(0);
                    w.write_str0_255(""); w.write_u16(0);
                    w.write_str0_255("mkpool"); w.write_str0_255("1"); w.write_str0_255("1"); w.write_str0_255("proxy");
                    self->send_sv2(sv2::MSG_SETUP_CONNECTION, w);
                    self->do_read();
                }));
        }));
}

void Sv2UpstreamClient::schedule_reconnect() {
    if (!running_.load()) return;
    const auto& uc = coin_.upstream;
    int lo = uc.reconnectMinSeconds > 0 ? uc.reconnectMinSeconds : 2;
    int hi = uc.reconnectMaxSeconds > 0 ? uc.reconnectMaxSeconds : 30;
    int delay = std::min(hi, lo << std::min(reconnect_backoff_, 5));
    reconnect_backoff_ = std::min(reconnect_backoff_ + 1, 6);
    reconnect_timer_.expires_after(std::chrono::seconds(std::max(delay, lo)));
    reconnect_timer_.async_wait(boost::asio::bind_executor(strand_,
        [self = shared_from_this()](const boost::system::error_code& ec) {
            if (!ec && self->running_.load()) self->connect();
        }));
}

void Sv2UpstreamClient::teardown(const std::string& reason) {
    { std::lock_guard lk(mu_); connected_ = setup_ok_ = channel_ok_ = false; }
    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    wq_.clear(); writing_ = false;
    if (running_.load()) {
        stat_reconnects_.fetch_add(1, std::memory_order_relaxed);
        spdlog::warn("[Sv2Upstream {}] link down ({}); reconnecting", coin_.name, reason);
        // Tell downstream sessions the link is down so they hold/await readiness.
        for (auto& s : live_sinks()) s->on_upstream_state(false);
        schedule_reconnect();
    }
}

void Sv2UpstreamClient::do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(read_buf_.data(), read_buf_.size()),
        boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code& ec, std::size_t n) { self->on_read(ec, n); }));
}

void Sv2UpstreamClient::on_read(const boost::system::error_code& ec, std::size_t n) {
    if (ec) { teardown(ec.message()); return; }
    in_.insert(in_.end(), read_buf_.data(), read_buf_.data() + n);
    if (in_.size() > (8u << 20)) { teardown("oversize buffer"); return; }
    consume_frames();
    if (connected_) do_read();
}

void Sv2UpstreamClient::consume_frames() {
    while (connected_) {
        if (!have_header_) {
            if (in_.size() < 6) return;
            sv2::Reader r(in_.data(), 6); cur_header_.deserialize(r);
            in_.erase(in_.begin(), in_.begin() + 6);
            if (!sv2::frame_length_ok(cur_header_.msg_length)) { teardown("oversize frame"); return; }
            have_header_ = true;
        }
        if (in_.size() < cur_header_.msg_length) return;
        std::vector<std::uint8_t> payload(in_.begin(), in_.begin() + cur_header_.msg_length);
        in_.erase(in_.begin(), in_.begin() + cur_header_.msg_length);
        have_header_ = false;
        try { sv2::Reader r(payload); handle_frame(cur_header_, r); }
        catch (const std::exception& e) {
            spdlog::warn("[Sv2Upstream {}] frame error type=0x{:02x}: {}", coin_.name, cur_header_.msg_type, e.what());
        }
    }
}

void Sv2UpstreamClient::send_sv2(std::uint8_t type, const sv2::Writer& payload) {
    auto frame = sv2::wrap_message(sv2::EXT_COMMON, type, payload);
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, f = std::move(frame)]() mutable {
        if (!self->connected_ && self->wq_.empty()) { /* allow SetupConnection pre-flag */ }
        self->wq_.push_back(std::move(f));
        if (!self->writing_) { self->writing_ = true; self->do_write(); }
    });
}

void Sv2UpstreamClient::do_write() {
    if (wq_.empty()) { writing_ = false; return; }
    auto self = shared_from_this();
    auto& front = wq_.front();
    boost::asio::async_write(socket_, boost::asio::buffer(front.data(), front.size()),
        boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code& ec, std::size_t) {
                if (ec) { self->writing_ = false; self->teardown("write: " + ec.message()); return; }
                self->wq_.pop_front();
                if (self->wq_.empty()) self->writing_ = false; else self->do_write();
            }));
}

// ---------------------------------------------------------------------------
// SV2 protocol
// ---------------------------------------------------------------------------
void Sv2UpstreamClient::handle_frame(const sv2::Header& h, sv2::Reader& r) {
    switch (h.msg_type) {
        case sv2::MSG_SETUP_CONNECTION_SUCCESS: {
            { std::lock_guard lk(mu_); setup_ok_ = true; }
            // Open an EXTENDED channel so we control a rollable extranonce to
            // subdivide across downstream V1 miners.
            sv2::Writer w;
            w.write_u32(1);  // request_id
            w.write_str0_255(coin_.upstream.endpoints.empty() ? "proxy" : coin_.upstream.endpoints.front().user);
            w.write_f32(1.0e12f);            // nominal hashrate hint
            std::array<std::uint8_t, 32> maxt; maxt.fill(0xff); w.write_u256(maxt);
            w.write_u16(static_cast<std::uint16_t>(nonce_bytes_ + 4)); // min extranonce size
            send_sv2(sv2::MSG_OPEN_EXTENDED_MINING_CHANNEL, w);
            spdlog::info("[Sv2Upstream {}] setup ok; opening extended channel", coin_.name);
            break;
        }
        case sv2::MSG_SETUP_CONNECTION_ERROR: {
            std::string code = r.read_str0_255();
            teardown("setup error: " + code);
            break;
        }
        case sv2::MSG_OPEN_EXTENDED_MINING_CHANNEL_SUCCESS: {
            sv2::OpenExtendedMiningChannelSuccess ok;
            ok.request_id = r.read_u32(); ok.channel_id = r.read_u32();
            ok.target = r.read_u256(); ok.extranonce_size = r.read_u16();
            ok.extranonce_prefix = r.read_b0_32(); ok.group_channel_id = r.read_u32();
            on_channel_open(ok);
            break;
        }
        case sv2::MSG_OPEN_MINING_CHANNEL_ERROR: {
            r.read_u32(); std::string code = r.read_str0_255();
            teardown("open channel error: " + code);
            break;
        }
        case sv2::MSG_NEW_EXTENDED_MINING_JOB: {
            sv2::NewExtendedMiningJob j;
            j.channel_id = r.read_u32(); j.job_id = r.read_u32();
            j.min_ntime = r.read_option_u32(); j.version = r.read_u32();
            j.version_rolling_allowed = r.read_bool();
            j.merkle_path = r.read_seq0_255_u256();
            j.coinbase_tx_prefix = r.read_b0_64k(); j.coinbase_tx_suffix = r.read_b0_64k();
            on_extended_job(j);
            break;
        }
        case sv2::MSG_SET_NEW_PREV_HASH: {
            sv2::SetNewPrevHash ph;
            ph.channel_id = r.read_u32(); ph.job_id = r.read_u32();
            ph.prev_hash = r.read_u256(); ph.min_ntime = r.read_u32(); ph.nbits = r.read_u32();
            on_set_prev_hash(ph);
            break;
        }
        case sv2::MSG_SET_TARGET: {
            r.read_u32(); auto t = r.read_u256(); on_set_target(t);
            break;
        }
        case sv2::MSG_SUBMIT_SHARES_SUCCESS: {
            r.read_u32(); std::uint32_t last_seq = r.read_u32(); on_submit_result(last_seq, true, "");
            break;
        }
        case sv2::MSG_SUBMIT_SHARES_ERROR: {
            r.read_u32(); std::uint32_t seq = r.read_u32(); std::string code = r.read_str0_255();
            on_submit_result(seq, false, code);
            break;
        }
        default:
            spdlog::debug("[Sv2Upstream {}] ignoring msg 0x{:02x}", coin_.name, h.msg_type);
            break;
    }
}

void Sv2UpstreamClient::on_channel_open(const sv2::OpenExtendedMiningChannelSuccess& ok) {
    {
        std::lock_guard lk(mu_);
        channel_ok_ = true; channel_id_ = ok.channel_id;
        extranonce_prefix_ = ok.extranonce_prefix; extranonce_size_ = ok.extranonce_size;
        diff_ = diff_from_target_le(ok.target);
        if (nonce_bytes_ >= extranonce_size_) nonce_bytes_ = std::max<std::uint8_t>(1, extranonce_size_ - 1);
    }
    spdlog::info("[Sv2Upstream {}] extended channel {} open: en_prefix={} en_size={} slot_bytes={} diff={:.0f}",
                 coin_.name, ok.channel_id, utils::bytes_to_hex({extranonce_prefix_.data(), extranonce_prefix_.size()}),
                 extranonce_size_, nonce_bytes_, diff_);
    notify_waiters();  // downstream sessions waiting for a ready link can attach now
}

void Sv2UpstreamClient::on_extended_job(const sv2::NewExtendedMiningJob& j) {
    {
        std::lock_guard lk(mu_);
        job_.sv2_job_id = j.job_id; job_.version = j.version;
        job_.version_rolling_allowed = j.version_rolling_allowed;
        job_.merkle_path = j.merkle_path; job_.cb_prefix = j.coinbase_tx_prefix;
        job_.cb_suffix = j.coinbase_tx_suffix; job_.have = true;
    }
    stat_jobs_.fetch_add(1, std::memory_order_relaxed);
    // A job carrying min_ntime is immediately active on the current prev-hash.
    if (j.min_ntime.has_value() && prev_.have) rebuild_and_broadcast(false);
}

void Sv2UpstreamClient::on_set_prev_hash(const sv2::SetNewPrevHash& ph) {
    { std::lock_guard lk(mu_);
      prev_.sv2_job_id = ph.job_id; prev_.prev_hash = ph.prev_hash;
      prev_.min_ntime = ph.min_ntime; prev_.nbits = ph.nbits; prev_.have = true; }
    if (job_.have) rebuild_and_broadcast(true);
}

void Sv2UpstreamClient::on_set_target(const std::array<std::uint8_t, 32>& target) {
    double d; { std::lock_guard lk(mu_); diff_ = diff_from_target_le(target); d = diff_; }
    for (auto& s : live_sinks()) s->on_upstream_diff(d);
}

void Sv2UpstreamClient::rebuild_and_broadcast(bool clean) {
    ProxyNotifyPtr notify;
    {
        std::lock_guard lk(mu_);
        if (!job_.have || !prev_.have) return;
        auto n = std::make_shared<ProxyNotify>();
        n->job_id = fmt::format("{:08x}", job_.sv2_job_id);
        std::string ph_hdr = utils::bytes_to_hex({prev_.prev_hash.data(), prev_.prev_hash.size()});
        n->prevhash_stratum = swap_words_hex(ph_hdr);           // -> stratum notify order
        n->coinbase1 = utils::bytes_to_hex({job_.cb_prefix.data(), job_.cb_prefix.size()});
        n->coinbase2 = utils::bytes_to_hex({job_.cb_suffix.data(), job_.cb_suffix.size()});
        n->merkle_branch.clear();
        for (auto& m : job_.merkle_path)
            n->merkle_branch.push_back(utils::bytes_to_hex({m.data(), m.size()}));
        n->version = job_.version;
        n->bits    = fmt::format("{:08x}", prev_.nbits);
        n->ntime   = fmt::format("{:08x}", prev_.min_ntime);
        n->mintime = prev_.min_ntime;
        n->clean   = clean;
        n->net_target_be = utils::target_from_bits_hex(n->bits);
        n->seq = ++notify_seq_;
        // Pre-render the mining.notify line the downstream V1 miner receives.
        std::string mb = "[";
        for (std::size_t i = 0; i < n->merkle_branch.size(); ++i) {
            if (i) mb.push_back(',');
            mb.push_back('"'); mb.append(n->merkle_branch[i]); mb.push_back('"');
        }
        mb.push_back(']');
        n->notify_line = fmt::format(
            R"({{"id":null,"method":"mining.notify","params":["{}","{}","{}","{}",{},"{:08x}","{}","{}",{}]}})" "\n",
            n->job_id, n->prevhash_stratum, n->coinbase1, n->coinbase2, mb,
            n->version, n->bits, n->ntime, clean ? "true" : "false");
        cur_notify_ = n; notify = n;
        // Map the V1 job id -> SV2 job id for submit translation.
        jobid_map_[n->job_id] = job_.sv2_job_id;
        jobid_order_.push_back(n->job_id);
        while (jobid_order_.size() > 32) { jobid_map_.erase(jobid_order_.front()); jobid_order_.pop_front(); }
    }
    for (auto& s : live_sinks()) s->on_upstream_job(notify);
}

// ---------------------------------------------------------------------------
// UpstreamSource
// ---------------------------------------------------------------------------
std::vector<std::shared_ptr<ProxyWorkSink>> Sv2UpstreamClient::live_sinks() const {
    std::vector<std::shared_ptr<ProxyWorkSink>> out;
    std::lock_guard lk(mu_);
    for (auto& [slot, w] : subscribers_) if (auto s = w.lock()) out.push_back(std::move(s));
    return out;
}

void Sv2UpstreamClient::notify_waiters() {
    std::vector<std::shared_ptr<ProxyWorkSink>> w;
    { std::lock_guard lk(mu_);
      for (auto& x : waiters_) if (auto s = x.lock()) w.push_back(std::move(s));
      waiters_.clear(); }
    for (auto& s : w) s->on_upstream_state(true);
}

ProxySlot Sv2UpstreamClient::attach(const std::weak_ptr<ProxyWorkSink>& sink) {
    std::lock_guard lk(mu_);
    if (!channel_ok_) return ProxySlot{};
    ProxySlot s;
    s.ok = true; s.link_id = 0; s.slot_id = next_slot_++;
    // slot id encoded big-endian in nonce_bytes_ bytes.
    std::string slot_hex;
    for (int i = nonce_bytes_ - 1; i >= 0; --i)
        slot_hex += fmt::format("{:02x}", (s.slot_id >> (8 * i)) & 0xff);
    s.slot_hex = slot_hex;
    s.en1_hex  = utils::bytes_to_hex({extranonce_prefix_.data(), extranonce_prefix_.size()}) + slot_hex;
    s.en2_size = extranonce_size_ > nonce_bytes_ ? (extranonce_size_ - nonce_bytes_) : 0;
    subscribers_[s.slot_id] = sink;
    return s;
}

void Sv2UpstreamClient::detach(int /*link_id*/, std::uint32_t slot_id) {
    std::lock_guard lk(mu_);
    subscribers_.erase(slot_id);
}

void Sv2UpstreamClient::register_waiter(const std::weak_ptr<ProxyWorkSink>& sink) {
    std::lock_guard lk(mu_);
    waiters_.push_back(sink);
}

ProxyNotifyPtr Sv2UpstreamClient::current_job(int /*link_id*/) const {
    std::lock_guard lk(mu_); return cur_notify_;
}

double Sv2UpstreamClient::current_diff(int /*link_id*/) const {
    std::lock_guard lk(mu_); return diff_;
}

bool Sv2UpstreamClient::any_ready() const {
    std::lock_guard lk(mu_); return channel_ok_;
}

void Sv2UpstreamClient::submit(int /*link_id*/, const UpstreamSubmit& s, SubmitCallback cb) {
    std::uint32_t sv2_job = 0, nonce = 0, ntime = 0, version = 0, chan = 0;
    std::vector<std::uint8_t> extranonce;
    {
        std::lock_guard lk(mu_);
        auto it = jobid_map_.find(s.job_id);
        if (!channel_ok_ || it == jobid_map_.end()) { if (cb) cb(false, "stale/notready"); return; }
        sv2_job = it->second; chan = channel_id_;
        version = s.version_bits ? *s.version_bits : job_.version;
        extranonce = utils::hex_to_bytes(s.en2_full_hex);   // slot || downstream en2
    }
    if (!parse_hex_u32(s.nonce_hex, nonce) || !parse_hex_u32(s.ntime_hex, ntime)) { if (cb) cb(false, "bad hex"); return; }

    auto self = shared_from_this();
    boost::asio::post(strand_, [self, chan, sv2_job, nonce, ntime, version, extranonce = std::move(extranonce), cb = std::move(cb)]() mutable {
        if (!self->connected_) { if (cb) cb(false, "link down"); return; }
        std::uint32_t seq = self->next_seq_++;
        if (cb) self->pending_[seq] = std::move(cb);
        sv2::Writer w;
        w.write_u32(chan); w.write_u32(seq); w.write_u32(sv2_job);
        w.write_u32(nonce); w.write_u32(ntime); w.write_u32(version);
        w.write_b0_32(extranonce);
        self->send_sv2(sv2::MSG_SUBMIT_SHARES_EXTENDED, w);
        self->stat_forwarded_.fetch_add(1, std::memory_order_relaxed);
    });
}

void Sv2UpstreamClient::on_submit_result(std::uint32_t seq, bool accepted, const std::string& err) {
    // Success acks every pending submit up to `seq`; Error rejects that one seq.
    std::vector<SubmitCallback> cbs;
    {
        // pending_ is strand-only.
        if (accepted) {
            for (auto it = pending_.begin(); it != pending_.end();) {
                if (it->first <= seq) { cbs.push_back(std::move(it->second)); it = pending_.erase(it); }
                else ++it;
            }
            stat_accepted_.fetch_add(cbs.size(), std::memory_order_relaxed);
        } else {
            auto it = pending_.find(seq);
            if (it != pending_.end()) { cbs.push_back(std::move(it->second)); pending_.erase(it); }
            stat_rejected_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    for (auto& cb : cbs) if (cb) cb(accepted, err);
}

UpstreamStatus Sv2UpstreamClient::status() const {
    UpstreamStatus st; st.mode = "sv2"; st.primary = 0;
    LinkStatus l; l.index = 0; l.endpoint = host_ + ":" + port_;
    { std::lock_guard lk(mu_);
      l.connected = connected_; l.authorized = channel_ok_; l.primary = true;
      l.difficulty = diff_; l.en2_size = extranonce_size_; l.sessions = subscribers_.size(); }
    l.jobs = stat_jobs_.load(); l.shares_forwarded = stat_forwarded_.load();
    l.shares_accepted = stat_accepted_.load(); l.shares_rejected = stat_rejected_.load();
    l.reconnects = stat_reconnects_.load();
    st.total_sessions = l.sessions; st.links.push_back(std::move(l));
    return st;
}

} // namespace mkpool
