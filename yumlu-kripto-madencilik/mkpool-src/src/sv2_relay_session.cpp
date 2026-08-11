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
// File:        sv2_relay_session.cpp
// Description: Stratum V2 (Noise) downstream session for the proxy role - SV2->V1 translation.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "sv2_relay_session.hpp"

#include "io_pool.hpp"
#include "merkle.hpp"
#include "metrics.hpp"
#include "rate_limiter.hpp"
#include "utils.hpp"

#include <cmath>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>   // SO_REUSEPORT
#include <spdlog/spdlog.h>

namespace mkpool {

namespace {

// Bitcoin difficulty-1 target (pdiff, 0xFFFF*2^208) -> 256-bit target, LE bytes.
// Identical maths to the solo SV2 path so a Bitaxe sees the exact same target.
std::array<std::uint8_t, 32> diff_to_target_le(double diff) {
    double max_target = 26959535291011309493156476344723991336010898738574164086137773096960.0;
    double t = (diff > 0.0) ? max_target / diff : max_target;
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 32; ++i) { out[i] = static_cast<std::uint8_t>(std::fmod(t, 256.0)); t = std::floor(t / 256.0); }
    return out;
}

// Stratum notify prevhash (8 words, word order reversed) -> header LE prevhash
// (reverse the 4 bytes within each word). Mirrors proxy_session so our local
// score reproduces the exact header the miner hashed.
std::string stratum_prevhash_to_header_le(std::string_view p) {
    if (p.size() != 64) return std::string(p);
    std::string out; out.resize(64);
    for (std::size_t w = 0; w < 8; ++w)
        for (std::size_t b = 0; b < 4; ++b) {
            out[w * 8 + b * 2]     = p[w * 8 + (3 - b) * 2];
            out[w * 8 + b * 2 + 1] = p[w * 8 + (3 - b) * 2 + 1];
        }
    return out;
}

std::uint32_t now_unix() {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // anonymous

Sv2RelaySession::Sv2RelaySession(boost::asio::io_context& io,
                                 const CoinConfig& coin,
                                 std::shared_ptr<RateLimiter> rl,
                                 std::shared_ptr<UpstreamSource> upstream,
                                 std::string authority_key_hex)
    : io_(io),
      strand_(boost::asio::make_strand(io.get_executor())),
      socket_(io),
      coin_(coin),
      rl_(std::move(rl)),
      upstream_(std::move(upstream)),
      authority_key_hex_(std::move(authority_key_hex)) {}

std::shared_ptr<ProxyWorkSink> Sv2RelaySession::sink_ptr() {
    return std::shared_ptr<ProxyWorkSink>(shared_from_this(), static_cast<ProxyWorkSink*>(this));
}

void Sv2RelaySession::start() {
    boost::system::error_code ec;
    socket_.set_option(boost::asio::ip::tcp::no_delay(true), ec);
    auto ep = socket_.remote_endpoint(ec);
    client_ip_ = ec ? std::string{"?"} : ep.address().to_string();
    metrics::inc_connections_accepted();
    // Noise is initialised lazily once we know the peer speaks encrypted SV2.
    do_read();
}

void Sv2RelaySession::shutdown() {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self] { self->close("shutdown"); });
}

void Sv2RelaySession::close(const char* reason) {
    if (closed_.exchange(true)) return;
    spdlog::debug("[sv2-proxy {}] disconnect: {}", client_ip_, reason);
    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    if (attached_ && upstream_) upstream_->detach(slot_.link_id, slot_.slot_id);
    if (rl_) rl_->on_disconnect(client_ip_);
    if (disconnect_handler_) disconnect_handler_(shared_from_this());
}

// ---------------------------------------------------------------------------
// transport
// ---------------------------------------------------------------------------
void Sv2RelaySession::do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(read_buf_.data(), read_buf_.size()),
        boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code& ec, std::size_t n) { self->on_read(ec, n); }));
}

void Sv2RelaySession::on_read(const boost::system::error_code& ec, std::size_t n) {
    if (ec) { close(ec.message().c_str()); return; }
    in_.insert(in_.end(), read_buf_.data(), read_buf_.data() + n);
    if (in_.size() > (4u << 20)) { close("oversize sv2 buffer"); return; }
    consume_frames();
    if (!closed_.load()) do_read();
}

void Sv2RelaySession::consume_frames() {
    // Determine transport mode from the first bytes: a plaintext SetupConnection
    // frame begins 00 00 00 (ext_type=0x0000, msg_type=SetupConnection=0x00); a
    // Noise act1 is 64 bytes of EllSwift key material (essentially never 00 00 00).
    if (!handshake_done_) {
        if (in_.size() < 6) return;
        const bool plaintext = (in_[0] == 0x00 && in_[1] == 0x00 && in_[2] == 0x00);
        if (plaintext) {
            noise_active_ = false;
            handshake_done_ = true;
        } else {
            // Encrypted: consume the 64-byte act1, reply act2, then frame loop.
            noise_active_ = true;
            if (in_.size() < 64) return;
            if (!noise_.initialize(authority_key_hex_)) { close("noise init failed"); return; }
            if (!noise_.process_act1(in_.data())) { close("bad act1"); return; }
            auto act2 = noise_.generate_act2();
            in_.erase(in_.begin(), in_.begin() + 64);
            raw_send(std::move(act2));
            handshake_done_ = true;
        }
    }

    while (!closed_.load()) {
        if (!have_header_) {
            if (noise_active_) {
                if (in_.size() < 22) return;
                std::uint8_t hdr[6];
                if (!noise_.decrypt(in_.data(), 22, hdr)) { close("hdr decrypt failed"); return; }
                sv2::Reader r(hdr, 6); cur_header_.deserialize(r);
                in_.erase(in_.begin(), in_.begin() + 22);
            } else {
                if (in_.size() < 6) return;
                sv2::Reader r(in_.data(), 6); cur_header_.deserialize(r);
                in_.erase(in_.begin(), in_.begin() + 6);
            }
            if (!sv2::frame_length_ok(cur_header_.msg_length)) { close("oversize frame"); return; }
            have_header_ = true;
        }

        std::vector<std::uint8_t> payload(cur_header_.msg_length);
        if (noise_active_) {
            std::size_t need = cur_header_.msg_length + 16;
            if (in_.size() < need) return;
            if (cur_header_.msg_length > 0 && !noise_.decrypt(in_.data(), need, payload.data())) {
                close("payload decrypt failed"); return;
            }
            in_.erase(in_.begin(), in_.begin() + need);
        } else {
            if (in_.size() < cur_header_.msg_length) return;
            std::memcpy(payload.data(), in_.data(), cur_header_.msg_length);
            in_.erase(in_.begin(), in_.begin() + cur_header_.msg_length);
        }
        have_header_ = false;

        try {
            sv2::Reader r(payload);
            handle_frame(cur_header_, r);
        } catch (const std::exception& e) {
            spdlog::warn("[sv2-proxy {}] frame handler error type=0x{:02x}: {}", client_ip_, cur_header_.msg_type, e.what());
        }
    }
}

void Sv2RelaySession::send_sv2(std::uint16_t ext, std::uint8_t type, const sv2::Writer& payload) {
    std::vector<std::uint8_t> frame = sv2::wrap_message(ext, type, payload);  // [6-byte header][payload]
    if (!noise_active_) { raw_send(std::move(frame)); return; }
    const std::size_t plen = frame.size() - 6;
    std::vector<std::uint8_t> enc(22 + plen + 16);
    if (!noise_.encrypt(frame.data(), 6, enc.data())) { close("hdr encrypt failed"); return; }
    if (!noise_.encrypt(frame.data() + 6, plen, enc.data() + 22)) { close("payload encrypt failed"); return; }
    raw_send(std::move(enc));
}

void Sv2RelaySession::raw_send(std::vector<std::uint8_t> bytes) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, b = std::move(bytes)]() mutable {
        if (self->closed_.load()) return;
        self->wq_.push_back(std::move(b));
        if (!self->writing_) { self->writing_ = true; self->do_write(); }
    });
}

void Sv2RelaySession::do_write() {
    if (closed_.load() || wq_.empty()) { writing_ = false; return; }
    auto self = shared_from_this();
    auto& front = wq_.front();
    boost::asio::async_write(socket_, boost::asio::buffer(front.data(), front.size()),
        boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code& ec, std::size_t) {
                if (ec) { self->writing_ = false; self->close(ec.message().c_str()); return; }
                self->wq_.pop_front();
                if (self->wq_.empty()) self->writing_ = false; else self->do_write();
            }));
}

// ---------------------------------------------------------------------------
// SV2 message dispatch
// ---------------------------------------------------------------------------
void Sv2RelaySession::handle_frame(const sv2::Header& h, sv2::Reader& r) {
    switch (h.msg_type) {
        case sv2::MSG_SETUP_CONNECTION:                 handle_setup_connection(r); break;
        case sv2::MSG_OPEN_STANDARD_MINING_CHANNEL:     handle_open_standard_channel(r); break;
        case sv2::MSG_SUBMIT_SHARES_STANDARD:           handle_submit_shares(r); break;
        case sv2::MSG_UPDATE_CHANNEL:                   break;  // hash-rate hint; diff is upstream-driven
        case sv2::MSG_CLOSE_CHANNEL:                    close("client closed channel"); break;
        default:
            spdlog::debug("[sv2-proxy {}] ignoring msg type 0x{:02x}", client_ip_, h.msg_type);
            break;
    }
}

void Sv2RelaySession::handle_setup_connection(sv2::Reader& r) {
    sv2::SetupConnection sc; sc.deserialize(r);
    // We only speak the Mining protocol; version rolling is left ALLOWED (flags=0)
    // so a fast miner has enough search space across a fixed per-job coinbase.
    sv2::SetupConnectionSuccess ok; ok.used_version = 2; ok.flags = 0;
    sv2::Writer w; ok.serialize(w);
    send_sv2(sv2::EXT_COMMON, sv2::MSG_SETUP_CONNECTION_SUCCESS, w);
    setup_done_ = true;
    spdlog::info("[sv2-proxy {}] setup ok (vendor='{}' fw='{}' noise={})",
                 client_ip_, sc.vendor, sc.firmware, noise_active_);
}

void Sv2RelaySession::handle_open_standard_channel(sv2::Reader& r) {
    sv2::OpenStandardMiningChannel oc; oc.deserialize(r);
    open_request_id_ = oc.request_id;
    user_identity_   = oc.user_identity;
    try_attach();
}

void Sv2RelaySession::try_attach() {
    if (!upstream_ || !upstream_->any_ready()) {
        upstream_->register_waiter(std::weak_ptr<ProxyWorkSink>(sink_ptr()));
        spdlog::info("[sv2-proxy {}] channel open held: no upstream link ready", client_ip_);
        return;
    }
    slot_ = upstream_->attach(std::weak_ptr<ProxyWorkSink>(sink_ptr()));
    if (!slot_.ok) {
        upstream_->register_waiter(std::weak_ptr<ProxyWorkSink>(sink_ptr()));
        spdlog::info("[sv2-proxy {}] channel open held: no slot yet", client_ip_);
        return;
    }
    attached_ = true;
    target_diff_ = upstream_->current_diff(slot_.link_id);
    if (target_diff_ <= 0) target_diff_ = 1.0;

    sv2::OpenStandardMiningChannelSuccess ok;
    ok.request_id       = open_request_id_;
    ok.channel_id       = channel_id_;
    ok.target           = diff_to_target_le(target_diff_);
    // For a standard channel the server owns the whole coinbase; expose the slot
    // bytes as the channel's extranonce prefix for transparency.
    ok.extranonce_prefix = utils::hex_to_bytes(slot_.en1_hex);
    ok.group_channel_id = 0;
    sv2::Writer w; ok.serialize(w);
    send_sv2(sv2::EXT_COMMON, sv2::MSG_OPEN_STANDARD_MINING_CHANNEL_SUCCESS, w);
    channel_open_ = true;
    spdlog::info("[sv2-proxy {}] channel {} open: link=L{} en1={} en2_size={} slot={} diff={:.0f}",
                 client_ip_, channel_id_, slot_.link_id, slot_.en1_hex, slot_.en2_size,
                 slot_.slot_hex, target_diff_);

    latest_up_job_ = upstream_->current_job(slot_.link_id);
    if (latest_up_job_) emit_job(latest_up_job_, true);
}

std::string Sv2RelaySession::current_fixed_en2() const {
    // Deterministic per-job extranonce2 (en2_size bytes) from the rotating counter.
    const std::size_t nbytes = slot_.en2_size;
    std::string hex; hex.reserve(nbytes * 2);
    for (std::size_t i = 0; i < nbytes; ++i) {
        std::uint8_t b = static_cast<std::uint8_t>((en2_counter_ >> (8 * (nbytes - 1 - i))) & 0xFF);
        static const char* H = "0123456789abcdef";
        hex.push_back(H[b >> 4]); hex.push_back(H[b & 0xF]);
    }
    return hex;
}

void Sv2RelaySession::emit_job(const ProxyNotifyPtr& job, bool /*clean*/) {
    if (!job || !channel_open_) return;
    // Rotate the fixed extranonce2 so each job gives the miner fresh coinbase-derived
    // work (nonce + version rolling then cover the rest of the space).
    ++en2_counter_;
    const std::string fixed_en2 = current_fixed_en2();
    const std::string full_extra = slot_.en1_hex + fixed_en2;               // up_en1 + slot + fixed_en2
    const std::string cb_hex     = utils::rebuild_coinbase(job->coinbase1, job->coinbase2, full_extra);
    const std::string cb_txid_le = utils::compute_coinbase_txid_le(cb_hex);
    const std::string mroot_le   = merkle::compute_root_le_hex(cb_txid_le, job->merkle_branch);

    const std::uint32_t job_id = next_job_id_++;
    jobs_[job_id] = JobRec{job, fixed_en2};
    job_order_.push_back(job_id);
    while (job_order_.size() > 16) { jobs_.erase(job_order_.front()); job_order_.pop_front(); }

    sv2::NewMiningJob mj;
    mj.channel_id = channel_id_;
    mj.job_id     = job_id;
    mj.version    = job->version;
    auto mr = utils::hex_to_bytes(mroot_le);
    if (mr.size() == 32) std::memcpy(mj.merkle_root.data(), mr.data(), 32);
    { sv2::Writer w; mj.serialize(w); send_sv2(sv2::EXT_COMMON, sv2::MSG_NEW_MINING_JOB, w); }

    sv2::SetNewPrevHash ph;
    ph.channel_id = channel_id_;
    ph.job_id     = job_id;
    ph.min_ntime  = job->mintime;
    ph.nbits      = 0;
    { auto bb = utils::hex_to_bytes(job->bits);
      if (bb.size() == 4) ph.nbits = (bb[0] << 24) | (bb[1] << 16) | (bb[2] << 8) | bb[3]; }
    // prev_hash into the header goes in internal (per-word reversed) order.
    auto ph_bytes = utils::hex_to_bytes(stratum_prevhash_to_header_le(job->prevhash_stratum));
    if (ph_bytes.size() == 32) std::memcpy(ph.prev_hash.data(), ph_bytes.data(), 32);
    { sv2::Writer w; ph.serialize(w); send_sv2(sv2::EXT_COMMON, sv2::MSG_SET_NEW_PREV_HASH, w); }
}

void Sv2RelaySession::send_set_target(double diff) {
    if (!channel_open_) return;
    sv2::SetTarget st; st.channel_id = channel_id_; st.maximum_target = diff_to_target_le(diff);
    sv2::Writer w; st.serialize(w);
    send_sv2(sv2::EXT_COMMON, sv2::MSG_SET_TARGET, w);
}

void Sv2RelaySession::handle_submit_shares(sv2::Reader& r) {
    sv2::SubmitSharesStandard s; s.deserialize(r);
    auto reject = [&](const char* code) {
        sv2::SubmitSharesError e; e.channel_id = channel_id_; e.sequence_number = s.sequence_number; e.error_code = code;
        sv2::Writer w; e.serialize(w); send_sv2(sv2::EXT_COMMON, sv2::MSG_SUBMIT_SHARES_ERROR, w);
        ++shares_rejected_; metrics::inc_share_rejected(code);
        if (rl_ && rl_->record_invalid_share(client_ip_)) close("ban: too many invalid shares");
    };
    if (!channel_open_ || !attached_) { reject("not-open"); return; }
    auto jit = jobs_.find(s.job_id);
    if (jit == jobs_.end()) { reject("stale-job"); metrics::inc_share_stale(); return; }
    const ProxyNotify& job = *jit->second.up;
    const std::string& fixed_en2 = jit->second.fixed_en2_hex;

    if (!utils::valid_ntime(s.ntime, job.mintime, now_unix())) { reject("ntime-invalid"); return; }

    // Reconstruct the exact header the miner hashed. In a standard channel the
    // miner rolls nonce/ntime/version over the server-fixed coinbase.
    const std::string ntime_s = fmt::format("{:08x}", s.ntime);
    const std::string nonce_s = fmt::format("{:08x}", s.nonce);
    const std::string full_extra = slot_.en1_hex + fixed_en2;
    const std::string cb_hex     = utils::rebuild_coinbase(job.coinbase1, job.coinbase2, full_extra);
    const std::string cb_txid_le = utils::compute_coinbase_txid_le(cb_hex);
    const std::string mroot_le   = merkle::compute_root_le_hex(cb_txid_le, job.merkle_branch);

    const std::string header_hex = utils::build_block_header(
        utils::le_hex_u32(s.version),
        stratum_prevhash_to_header_le(job.prevhash_stratum),
        mroot_le,
        utils::byte_reverse_hex(ntime_s),
        utils::byte_reverse_hex(job.bits),
        utils::byte_reverse_hex(nonce_s));
    auto hb = utils::hex_to_bytes(header_hex);
    auto sha = utils::sha256d_arr({hb.data(), hb.size()});
    std::array<std::uint8_t, 32> hash_be{};
    for (std::size_t i = 0; i < 32; ++i) hash_be[i] = sha[31 - i];

    auto le_cmp = [](const auto& a, const auto& b) {
        for (std::size_t i = 0; i < 32; ++i) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        return 0;
    };
    const bool meets_network = le_cmp(hash_be, job.net_target_be) <= 0;

    double share_diff = 0.0;
    { long double hv = 0.0L, base = 1.0L;
      for (int i = 31; i >= 0; --i) { hv += (long double)hash_be[i] * base; base *= 256.0L; }
      long double maxt = (long double)0xFFFF * std::pow(2.0L, 208.0L);
      if (hv > 0.0L) share_diff = (double)(maxt / hv); }

    const double threshold = target_diff_ > 0 ? target_diff_ : 1.0;
    if (share_diff < threshold * 0.99 && !meets_network) { reject("low-difficulty"); return; }

    // Forward upstream as a V1 mining.submit. The full upstream extranonce2 is
    // slot || fixed_en2; version rolling is passed through as the 6th param.
    UpstreamSubmit us{ job.job_id, slot_.slot_hex + fixed_en2, ntime_s, nonce_s, std::optional<std::uint32_t>{s.version} };
    if (meets_network)
        spdlog::warn("[sv2-proxy {}] share solves a BLOCK (share_diff={:.4f}); forwarding upstream", client_ip_, share_diff);
    auto wself = weak_from_this();
    upstream_->submit(slot_.link_id, us, [wself](bool accepted, const std::string& err) {
        if (auto self = wself.lock(); self && !accepted)
            spdlog::info("[sv2-proxy] upstream rejected a forwarded share: {}", err.empty() ? "?" : err);
    });
    ++shares_forwarded_;

    sv2::SubmitSharesSuccess ok;
    ok.channel_id = channel_id_; ok.last_sequence_number = s.sequence_number;
    ok.new_submits_accepted_count = 1; ok.new_shares_sum = (std::uint64_t)std::llround(threshold);
    sv2::Writer w; ok.serialize(w); send_sv2(sv2::EXT_COMMON, sv2::MSG_SUBMIT_SHARES_SUCCESS, w);
    ++shares_accepted_;
    if (rl_) rl_->clear_invalid(client_ip_);
    metrics::inc_share_accepted();
}

// ---------------------------------------------------------------------------
// upstream events (posted onto this session's strand)
// ---------------------------------------------------------------------------
void Sv2RelaySession::on_upstream_job(ProxyNotifyPtr job) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, this, job = std::move(job)]() mutable {
        if (closed_.load() || !job) return;
        latest_up_job_ = job;
        if (channel_open_) emit_job(job, false);
    });
}

void Sv2RelaySession::on_upstream_diff(double diff) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, this, diff] {
        if (closed_.load()) return;
        target_diff_ = diff > 0 ? diff : target_diff_;
        if (channel_open_) send_set_target(target_diff_);
    });
}

void Sv2RelaySession::on_upstream_extranonce(std::string en1_hex, std::size_t en2_size) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, this, en1_hex = std::move(en1_hex), en2_size]() mutable {
        if (closed_.load()) return;
        slot_.en1_hex = std::move(en1_hex); slot_.en2_size = en2_size;
        if (channel_open_ && latest_up_job_) emit_job(latest_up_job_, true);
    });
}

void Sv2RelaySession::on_upstream_state(bool up) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, this, up] {
        if (closed_.load()) return;
        if (up && setup_done_ && !channel_open_) try_attach();
    });
}

void Sv2RelaySession::on_rebind(ProxySlot slot, ProxyNotifyPtr job, double diff) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, this, slot = std::move(slot), job = std::move(job), diff]() mutable {
        if (closed_.load() || !slot.ok) return;
        slot_ = slot; attached_ = true;
        target_diff_ = diff > 0 ? diff : target_diff_;
        send_set_target(target_diff_);
        if (job) { latest_up_job_ = job; emit_job(job, true); }
    });
}

// ===========================================================================
// Sv2RelayListener
// ===========================================================================
Sv2RelayListener::Sv2RelayListener(boost::asio::io_context& accept_io,
                                   IoPool& workers,
                                   std::string bind_addr,
                                   std::uint16_t port,
                                   std::shared_ptr<RateLimiter> rl,
                                   const CoinConfig& coin,
                                   std::shared_ptr<UpstreamSource> upstream,
                                   std::string authority_key_hex,
                                   std::string label)
    : accept_io_(accept_io), workers_(workers), bind_addr_(std::move(bind_addr)), port_(port),
      rl_(std::move(rl)), coin_(coin), upstream_(std::move(upstream)),
      authority_key_hex_(std::move(authority_key_hex)), label_(std::move(label)),
      acceptor_(accept_io) {}

void Sv2RelayListener::start() {
    namespace asio = boost::asio;
    using boost::asio::ip::tcp;
    boost::system::error_code ec;
    tcp::endpoint ep(asio::ip::make_address(bind_addr_, ec), port_);
    if (ec) { spdlog::error("[{}] bad listen address '{}': {}", label_, bind_addr_, ec.message()); return; }
    acceptor_.open(ep.protocol(), ec);
    if (ec) { spdlog::error("[{}] open: {}", label_, ec.message()); return; }
    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
#ifdef SO_REUSEPORT
    { const int on = 1;
      if (::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0)
          spdlog::warn("[{}] SO_REUSEPORT not set on :{} ({})", label_, port_, std::strerror(errno)); }
#endif
    acceptor_.set_option(tcp::no_delay(true), ec);
    acceptor_.bind(ep, ec);
    if (ec) { spdlog::error("[{}] bind {}:{} failed: {}", label_, bind_addr_, port_, ec.message()); return; }
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) { spdlog::error("[{}] listen: {}", label_, ec.message()); return; }
    running_.store(true);
    spdlog::info("[{}] listening on {}:{} (Stratum V2)", label_, bind_addr_, port_);
    do_accept();
}

void Sv2RelayListener::stop() {
    running_.store(false);
    boost::system::error_code ec;
    acceptor_.close(ec);
}

void Sv2RelayListener::do_accept() {
    if (!running_.load()) return;
    auto& worker = workers_.next();
    auto session = std::make_shared<Sv2RelaySession>(worker, coin_, rl_, upstream_, authority_key_hex_);
    auto self = shared_from_this();
    acceptor_.async_accept(session->socket(),
        [self, session](const boost::system::error_code& ec) {
            if (!self->running_.load()) return;
            if (ec) {
                if (ec != boost::asio::error::operation_aborted)
                    spdlog::warn("[{}] accept: {}", self->label_, ec.message());
                self->do_accept();
                return;
            }
            boost::system::error_code ep_ec;
            auto remote = session->socket().remote_endpoint(ep_ec);
            std::string ip = ep_ec ? std::string{"?"} : remote.address().to_string();
            if (self->rl_ && !self->rl_->allow_connection(ip)) {
                metrics::inc_connections_rejected("rate");
                boost::system::error_code cec; session->socket().close(cec);
                self->do_accept();
                return;
            }
            try { session->start(); } catch (const std::exception& e) {
                spdlog::error("[{}] session start failed: {}", self->label_, e.what());
            }
            self->do_accept();
        });
}

} // namespace mkpool
