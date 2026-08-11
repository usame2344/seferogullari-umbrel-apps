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
// File:        proxy_session.cpp
// Description: Downstream miner session for the proxy role.
// Created:     2026-07-19
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "proxy_session.hpp"

#include "merkle.hpp"
#include "metrics.hpp"
#include "stratum_protocol.hpp"
#include "utils.hpp"

#include <array>
#include <cmath>
#include <spdlog/spdlog.h>

namespace mkpool {

namespace {

// JSON-RPC id echoed back as a literal (null / number / quoted string). Mirrors
// the solo path's hardening so a string id can never throw on a busy proxy.
std::string safe_id_json(const nlohmann::json& msg) {
    auto it = msg.find("id");
    if (it == msg.end() || it->is_null()) return "null";
    try {
        if (it->is_number_integer())  return fmt::format("{}", it->get<std::int64_t>());
        if (it->is_number_unsigned()) return fmt::format("{}", it->get<std::uint64_t>());
        if (it->is_number_float())    return fmt::format("{}", it->get<double>());
        if (it->is_string()) {
            const auto& s = it->get_ref<const std::string&>();
            std::string o; o.reserve(s.size() + 2);
            o.push_back('"');
            for (char c : s) {
                if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
                else if ((unsigned char)c < 0x20) o.push_back('?');
                else o.push_back(c);
            }
            o.push_back('"');
            return o;
        }
    } catch (...) {}
    return "null";
}

bool parse_hex_u32(std::string_view h, std::uint32_t& out) {
    if (h.size() != 8) return false;
    try { out = static_cast<std::uint32_t>(std::stoul(std::string(h), nullptr, 16)); }
    catch (...) { return false; }
    return true;
}

std::uint32_t now_unix() {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// Convert a Stratum notify prevhash (8 32-bit words, word order reversed) into
// the block header's little-endian prevhash by reversing the 4 bytes within each
// word (the universal miner "flip_32"). This exactly reproduces the header the
// downstream miner built from the relayed notify, so our local score matches.
std::string stratum_prevhash_to_header_le(std::string_view p) {
    if (p.size() != 64) return std::string(p);
    std::string out;
    out.resize(64);
    for (std::size_t w = 0; w < 8; ++w) {
        for (std::size_t b = 0; b < 4; ++b) {
            out[w * 8 + b * 2]     = p[w * 8 + (3 - b) * 2];
            out[w * 8 + b * 2 + 1] = p[w * 8 + (3 - b) * 2 + 1];
        }
    }
    return out;
}

} // anonymous

ProxyDownstreamSession::ProxyDownstreamSession(boost::asio::io_context& io,
                                               const CoinConfig& coin,
                                               std::shared_ptr<RateLimiter> rl,
                                               std::shared_ptr<UpstreamSource> upstream,
                                               std::shared_ptr<boost::asio::ssl::context> tls_ctx)
    : RelaySession(io, std::move(rl), std::move(tls_ctx)),
      coin_(coin),
      upstream_(std::move(upstream)) {
    session_id_ = utils::generate_session_id();
}

std::shared_ptr<ProxyWorkSink> ProxyDownstreamSession::sink_ptr() {
    return std::shared_ptr<ProxyWorkSink>(shared_from_this(), static_cast<ProxyWorkSink*>(this));
}

// ---------------------------------------------------------------------------
// Line dispatch
// ---------------------------------------------------------------------------
void ProxyDownstreamSession::on_line(std::string_view line) {
    nlohmann::json msg;
    try { msg = nlohmann::json::parse(line); }
    catch (...) {
        send_line(R"({"id":null,"result":null,"error":[20,"JSON parse error",null]})" "\n");
        return;
    }
    if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string()) {
        send_line(R"({"id":null,"result":null,"error":[20,"missing method",null]})" "\n");
        return;
    }
    try {
        const std::string method = msg["method"].get<std::string>();
        using namespace stratum;
        if      (method == kMethodConfigure)          handle_configure(msg);
        else if (method == kMethodSubscribe)          handle_subscribe(msg);
        else if (method == kMethodAuthorize)          handle_authorize(msg);
        else if (method == kMethodSubmit)             handle_submit(msg);
        else if (method == kMethodExtranonceSub) {
            extranonce_subscribed_ = true;
            send_line(fmt::format(R"({{"id":{},"result":true,"error":null}})" "\n", safe_id_json(msg)));
        } else if (method == kMethodSuggestDifficulty || method == kMethodSuggestTarget) {
            // A proxy mirrors the upstream difficulty; suggestions are accepted
            // but not acted on. No reply is required by the protocol.
        } else {
            send_line(fmt::format(
                R"({{"id":{},"result":null,"error":[20,"unsupported in proxy mode",null]}})" "\n",
                safe_id_json(msg)));
        }
    } catch (const std::exception& e) {
        spdlog::warn("[proxy {}] handler exception: {}", client_ip_, e.what());
        send_line(fmt::format(
            R"({{"id":{},"result":null,"error":[20,"malformed request",null]}})" "\n",
            safe_id_json(msg)));
    }
}

void ProxyDownstreamSession::handle_configure(const nlohmann::json& msg) {
    const auto id_json = safe_id_json(msg);
    auto params = nlohmann::json::array();
    if (auto it = msg.find("params"); it != msg.end() && it->is_array()) params = *it;
    // Advertise the coin's configured rolling mask (upstream is asked for the
    // same mask, so a granted downstream bit is honoured upstream).
    auto neg = stratum::negotiate_configure(params,
        coin_.enableVersionRolling ? coin_.versionRollingMask : 0);
    version_rolling_ = neg.version_rolling && neg.version_mask != 0;
    version_mask_    = neg.version_mask;

    std::string body;
    auto append = [&body](std::string_view kv) { if (!body.empty()) body.push_back(','); body.append(kv); };
    if (neg.version_rolling_requested) {
        if (neg.version_rolling)
            append(fmt::format(R"("version-rolling":true,"version-rolling.mask":"{:08x}")", neg.version_mask));
        else
            append(R"("version-rolling":false)");
    }
    if (neg.subscribe_extranonce) { append(R"("subscribe-extranonce":true)"); extranonce_subscribed_ = true; }
    send_line(fmt::format(R"({{"id":{},"result":{{{}}},"error":null}})" "\n", id_json, body));
}

void ProxyDownstreamSession::handle_subscribe(const nlohmann::json& msg) {
    if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
        msg["params"][0].is_string()) {
        user_agent_ = msg["params"][0].get<std::string>();
        if (user_agent_.size() > 256) user_agent_.resize(256);
    }
    pending_subscribe_id_ = safe_id_json(msg);
    try_attach_and_reply();
}

void ProxyDownstreamSession::try_attach_and_reply() {
    if (subscribed_) { send_subscribe_reply(); return; }
    if (!upstream_ || !upstream_->any_ready()) {
        pending_subscribe_ = true;   // complete once a link comes up
        upstream_->register_waiter(std::weak_ptr<ProxyWorkSink>(sink_ptr()));
        spdlog::info("[proxy {}] subscribe held: no upstream link ready", client_ip_);
        return;
    }
    slot_ = upstream_->attach(std::weak_ptr<ProxyWorkSink>(sink_ptr()));
    if (!slot_.ok) {
        // No link ready between the check and the attach: wait for one.
        pending_subscribe_ = true;
        upstream_->register_waiter(std::weak_ptr<ProxyWorkSink>(sink_ptr()));
        spdlog::info("[proxy {}] subscribe held: no slot yet", client_ip_);
        return;
    }
    attached_ = true;
    pending_subscribe_ = false;
    upstream_diff_ = upstream_->current_diff(slot_.link_id);
    target_diff_   = upstream_diff_;
    send_subscribe_reply();
    subscribed_ = true;
    spdlog::info("[proxy {}] subscribed UA='{}' link=L{} en1={} en2_size={} slot={}",
                 client_ip_, user_agent_, slot_.link_id, slot_.en1_hex, slot_.en2_size, slot_.slot_hex);
    send_set_difficulty(target_diff_);
    if (version_rolling_ && version_mask_)
        send_line(fmt::format(R"({{"id":null,"method":"mining.set_version_mask","params":["{:08x}"]}})" "\n",
                              version_mask_));
    cur_job_ = upstream_->current_job(slot_.link_id);
    if (cur_job_) remember_job(cur_job_);
}

void ProxyDownstreamSession::send_subscribe_reply() {
    send_line(fmt::format(
        R"({{"id":{},"result":[[["mining.set_difficulty","{}"],["mining.notify","{}"]],"{}",{}],"error":null}})" "\n",
        pending_subscribe_id_, session_id_, session_id_, slot_.en1_hex, slot_.en2_size));
}

void ProxyDownstreamSession::send_set_difficulty(double d) {
    double rounded = std::round(d > 0 ? d : 1.0);
    send_line(fmt::format(R"({{"id":null,"method":"mining.set_difficulty","params":[{:.0f}]}})" "\n", rounded));
}

void ProxyDownstreamSession::handle_authorize(const nlohmann::json& msg) {
    const auto id_json = safe_id_json(msg);
    if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
        msg["params"][0].is_string()) {
        worker_name_ = msg["params"][0].get<std::string>();
        if (worker_name_.size() > 192) worker_name_.resize(192);
    }
    // The proxy authorizes upstream as its own single user; downstream worker
    // names are accepted for identification only (upstream owns payout).
    send_line(fmt::format(R"({{"id":{},"result":true,"error":null}})" "\n", id_json));
    authorized_ = true;
    spdlog::info("[proxy {}] authorized worker='{}'", client_ip_, worker_name_);
    if (subscribed_ && cur_job_) emit_current_job(true);
}

void ProxyDownstreamSession::remember_job(const ProxyNotifyPtr& job) {
    if (!job) return;
    if (recent_jobs_.emplace(job->job_id, job).second) {
        recent_order_.push_back(job->job_id);
        while (recent_order_.size() > 8) {
            recent_jobs_.erase(recent_order_.front());
            recent_order_.pop_front();
        }
    }
}

void ProxyDownstreamSession::emit_current_job(bool force_clean) {
    if (!cur_job_) return;
    std::string out = cur_job_->notify_line;   // already ends in '\n'
    if (force_clean && out.size() >= 9 && out.compare(out.size() - 9, 9, ",false]}\n") == 0)
        out.replace(out.size() - 9, 9, ",true]}\n");
    send_line(std::move(out));
}

// ---------------------------------------------------------------------------
// Share handling: local score, forward qualifying shares upstream
// ---------------------------------------------------------------------------
void ProxyDownstreamSession::handle_submit(const nlohmann::json& msg) {
    const auto id_json = safe_id_json(msg);
    auto reject = [&](const char* reason, const char* metric, int code = 20) {
        spdlog::warn("[proxy {}] share rejected ({}): {}", client_ip_, metric, reason);
        send_line(fmt::format(R"({{"id":{},"result":null,"error":[{},"{}",null]}})" "\n", id_json, code, reason));
        ++shares_rejected_;
        metrics::inc_share_rejected(metric);
        if (rl_ && rl_->record_invalid_share(client_ip_)) close("ban: too many invalid shares");
    };

    if (!authorized_) { reject("not authorized", "unauthorized", 24); return; }
    if (!subscribed_ || !attached_) { reject("not subscribed", "not-subscribed", 25); return; }
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].size() < 5) {
        reject("bad params", "bad-params"); return;
    }
    const auto& p = msg["params"];
    // params = [worker, job_id, extranonce2, ntime, nonce, (version_bits)].
    // The worker name (p[0]) is not used: the upstream owns payout, so downstream
    // worker names are cosmetic and never validated here.
    std::string jobId, en2, ntime_s, nonce_s;
    std::optional<std::uint32_t> version_bits;
    try {
        jobId   = p[1].get<std::string>();
        en2     = p[2].get<std::string>();
        ntime_s = p[3].get<std::string>();
        nonce_s = p[4].get<std::string>();
        if (p.size() >= 6 && p[5].is_string()) {
            std::uint32_t v;
            if (parse_hex_u32(p[5].get<std::string>(), v)) version_bits = v;
        }
    } catch (...) { reject("bad params types", "bad-params"); return; }

    if (!utils::valid_hex(en2) || !utils::valid_hex(ntime_s) || !utils::valid_hex(nonce_s) ||
        ntime_s.size() != 8 || nonce_s.size() != 8) {
        reject("invalid hex", "bad-hex"); return;
    }
    if (en2.size() != slot_.en2_size * 2) { reject("bad extranonce2 size", "bad-en2-size"); return; }

    auto jit = recent_jobs_.find(jobId);
    if (jit == recent_jobs_.end()) { reject("job not found", "stale", 21); metrics::inc_share_stale(); return; }
    const ProxyNotify& job = *jit->second;

    std::uint32_t submitted_ntime;
    if (!parse_hex_u32(ntime_s, submitted_ntime)) { reject("bad ntime", "bad-hex"); return; }
    if (!utils::valid_ntime(submitted_ntime, job.mintime, now_unix())) {
        reject("ntime out of range", "ntime"); return;
    }

    // Version rolling: fold the granted bits into the template version.
    std::uint32_t version = job.version;
    if (version_bits) {
        if (!version_rolling_ || version_mask_ == 0) { reject("version-rolling not negotiated", "vrb"); return; }
        version = (job.version & ~version_mask_) | (*version_bits & version_mask_);
    }

    // Rebuild the exact coinbase/merkle-root/header the miner hashed. The full
    // extranonce is downstream_en1 (which embeds the proxy slot) + submitted en2.
    const std::string full_extra = slot_.en1_hex + en2;
    std::string cb_hex      = utils::rebuild_coinbase(job.coinbase1, job.coinbase2, full_extra);
    std::string cb_txid_le  = utils::compute_coinbase_txid_le(cb_hex);
    std::string merkle_root = merkle::compute_root_le_hex(cb_txid_le, job.merkle_branch);

    std::string header_hex = utils::build_block_header(
        utils::le_hex_u32(version),
        stratum_prevhash_to_header_le(job.prevhash_stratum),
        merkle_root,
        utils::byte_reverse_hex(ntime_s),
        utils::byte_reverse_hex(job.bits),
        utils::byte_reverse_hex(nonce_s));
    auto header_bytes = utils::hex_to_bytes(header_hex);

    std::array<std::uint8_t, 32> sha_out{};
    if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin)
        sha_out = utils::scrypt_1024_1_1_32({header_bytes.data(), header_bytes.size()});
    else
        sha_out = utils::sha256d_arr({header_bytes.data(), header_bytes.size()});

    std::array<std::uint8_t, 32> hash_be{};
    for (std::size_t i = 0; i < 32; ++i) hash_be[i] = sha_out[31 - i];

    auto le_cmp = [](const auto& a, const auto& b) {
        for (std::size_t i = 0; i < 32; ++i) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        return 0;
    };
    const bool meets_network = le_cmp(hash_be, job.net_target_be) <= 0;

    double share_diff = 0.0;
    {
        long double hv = 0.0L, base = 1.0L;
        for (int i = 31; i >= 0; --i) { hv += (long double)hash_be[i] * base; base *= 256.0L; }
        long double maxt = (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin)
                               ? (long double)0xFFFF * std::pow(2.0L, 224.0L)
                               : (long double)0xFFFF * std::pow(2.0L, 208.0L);
        if (hv > 0.0L) share_diff = (double)(maxt / hv);
    }

    const double tol = 0.99;
    const double threshold = target_diff_ > 0 ? target_diff_ : 1.0;

    // Below threshold: only a genuine network block is still worth forwarding
    // (low-diff chains/testnets can put the network target under the pool diff).
    if (share_diff < threshold * tol) {
        if (meets_network) {
            spdlog::warn("[proxy {}] low-diff share solves a BLOCK; forwarding upstream (share_diff={:.4f})",
                         client_ip_, share_diff);
            UpstreamSubmit us{jobId, slot_.slot_hex + en2, ntime_s, nonce_s, version_bits};
            upstream_->submit(slot_.link_id, us, {});
            ++shares_forwarded_;
        }
        reject("low difficulty share", "low-diff", 23);
        return;
    }

    // Accepted downstream. Forward upstream and relay the verdict path in metrics.
    if (meets_network)
        spdlog::warn("[proxy {}] share solves a BLOCK (share_diff={:.4f}); forwarding upstream",
                     client_ip_, share_diff);

    UpstreamSubmit us{jobId, slot_.slot_hex + en2, ntime_s, nonce_s, version_bits};
    auto wself = weak_from_this();
    upstream_->submit(slot_.link_id, us, [wself](bool accepted, const std::string& err) {
        if (auto self = wself.lock()) {
            if (!accepted)
                spdlog::info("[proxy] upstream rejected a forwarded share: {}", err.empty() ? "?" : err);
        }
    });
    ++shares_forwarded_;

    send_line(fmt::format(R"({{"id":{},"result":true,"error":null}})" "\n", id_json));
    ++shares_accepted_;
    if (rl_) rl_->clear_invalid(client_ip_);
    metrics::inc_share_accepted();
}

// ---------------------------------------------------------------------------
// Upstream events (posted onto this session's strand)
// ---------------------------------------------------------------------------
void ProxyDownstreamSession::on_upstream_job(ProxyNotifyPtr job) {
    auto self = shared_from_this();
    boost::asio::post(strand(), [self, this, job = std::move(job)]() mutable {
        if (closed_.load() || !job) return;
        cur_job_ = job;
        remember_job(cur_job_);
        if (subscribed_ && authorized_) emit_current_job(false);
    });
}

void ProxyDownstreamSession::on_upstream_diff(double diff) {
    auto self = shared_from_this();
    boost::asio::post(strand(), [self, this, diff] {
        if (closed_.load()) return;
        upstream_diff_ = diff;
        target_diff_   = diff;   // mirror: every accepted downstream share forwards
        if (subscribed_) send_set_difficulty(diff);
    });
}

void ProxyDownstreamSession::on_upstream_extranonce(std::string en1_hex, std::size_t en2_size) {
    auto self = shared_from_this();
    boost::asio::post(strand(), [self, this, en1_hex = std::move(en1_hex), en2_size]() mutable {
        if (closed_.load()) return;
        slot_.en1_hex  = std::move(en1_hex);
        slot_.en2_size = en2_size;
        if (subscribed_) {
            send_line(fmt::format(
                R"({{"id":null,"method":"mining.set_extranonce","params":["{}",{}]}})" "\n",
                slot_.en1_hex, slot_.en2_size));
            emit_current_job(true);
        }
    });
}

void ProxyDownstreamSession::on_upstream_state(bool up) {
    auto self = shared_from_this();
    boost::asio::post(strand(), [self, this, up] {
        if (closed_.load()) return;
        if (up && pending_subscribe_) try_attach_and_reply();
    });
}

// Migrated to a different upstream link (hot-standby failover / active-active
// rebalance): adopt the new slot binding + difficulty + job, then re-key the
// miner via set_extranonce and push a clean job so it starts on the new link
// with no lost work and no visible interruption.
void ProxyDownstreamSession::on_rebind(ProxySlot slot, ProxyNotifyPtr job, double diff) {
    auto self = shared_from_this();
    boost::asio::post(strand(), [self, this, slot = std::move(slot), job = std::move(job), diff]() mutable {
        if (closed_.load() || !slot.ok) return;
        slot_ = slot;
        attached_ = true;
        upstream_diff_ = diff > 0 ? diff : upstream_diff_;
        target_diff_   = upstream_diff_;
        // A miner that negotiated extranonce subscription can be re-keyed in place
        // for a truly seamless migration (no reconnect, no lost work). One that
        // cannot is dropped so it auto-reconnects onto the new primary link with a
        // fresh extranonce (universal, ~1s) - the pre-allocated slot is released
        // by on_closed().
        if (extranonce_subscribed_) {
            spdlog::info("[proxy {}] rebound to L{} en1={} en2_size={} (seamless set_extranonce)",
                         client_ip_, slot_.link_id, slot_.en1_hex, slot_.en2_size);
            send_line(fmt::format(
                R"({{"id":null,"method":"mining.set_extranonce","params":["{}",{}]}})" "\n",
                slot_.en1_hex, slot_.en2_size));
            send_set_difficulty(target_diff_);
            if (job) { cur_job_ = job; remember_job(cur_job_); emit_current_job(true); }
        } else {
            spdlog::info("[proxy {}] link failed; dropping miner to reconnect onto L{} "
                         "(no extranonce.subscribe)", client_ip_, slot_.link_id);
            close("upstream link failed - reconnect to standby");
        }
    });
}

void ProxyDownstreamSession::on_closed() {
    if (attached_ && upstream_) upstream_->detach(slot_.link_id, slot_.slot_id);
}

} // namespace mkpool
