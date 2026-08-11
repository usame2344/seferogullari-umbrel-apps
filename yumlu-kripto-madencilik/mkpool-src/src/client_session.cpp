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
// File:        client_session.cpp
// Description: Per-miner Stratum session: subscribe/authorize, share validation and coinbase build (V1, TLS and SV2).
// Created:     2026-05-17
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "client_session.hpp"

#include "address.hpp"
#include "blacklist.hpp"
#include "database.hpp"
#include "db_worker.hpp"
#include "merkle.hpp"
#include "metrics.hpp"
#include "stratum_protocol.hpp"
#include "utils.hpp"
#include "equihash.hpp"
#include "zcash.hpp"
#include "sv2_messages.hpp"

#include <chrono>
#include <ctime>
#include <spdlog/spdlog.h>

namespace mkpool {

namespace {

// Insert witness marker/flag (0x00 0x01) after the 4-byte version field and a
// 32-byte zero "reserved value" witness item before the 4-byte locktime field.
// Required to submit blocks that contain segwit transactions.
std::string segwitify_coinbase(std::string_view cb_hex) {
    if (cb_hex.size() < 16) return std::string(cb_hex);
    constexpr std::size_t version_hex = 8;     // 4 bytes
    constexpr std::size_t locktime_hex = 8;    // 4 bytes
    std::string out;
    out.reserve(cb_hex.size() + 4 + 68);
    out.append(cb_hex.substr(0, version_hex));    // version
    out.append("0001");                            // marker + flag
    out.append(cb_hex.substr(version_hex, cb_hex.size() - version_hex - locktime_hex));
    out.append("01");                              // witness stack count = 1
    out.append("20");                              // push 32 bytes
    out.append(64, '0');                            // reserved value
    out.append(cb_hex.substr(cb_hex.size() - locktime_hex));
    return out;
}

// Build a complete, legacy-serialized DOGE coinbase transaction paying the
// miner (and the operator donation). Dogecoin has no segwit, so this is the
// final coinbase used both for the merkle root and for block assembly.
std::string build_doge_coinbase(std::int32_t height,
                                std::string_view sig,
                                std::int64_t value,
                                std::string_view miner_script,
                                std::string_view donation_script,
                                double donation_percent) {
    std::string script_sig = utils::encode_bip34_height(height);
    if (!sig.empty()) script_sig += utils::encode_push_ascii(sig);
    std::size_t ss_len = script_sig.size() / 2;
    if (ss_len > 0xfc) return {};  // would need a longer varint; never happens here

    std::int64_t donation = 0;
    if (donation_percent > 0.0 && !donation_script.empty()) {
        donation = static_cast<std::int64_t>(std::llround((double)value * donation_percent / 100.0));
        if (donation < 0 || donation > value) donation = 0;
    }
    std::int64_t miner_value = value - donation;
    int outputs = (donation > 0 && !donation_script.empty()) ? 2 : 1;

    std::string cb;
    cb.reserve(200);
    cb += "01000000";              // version
    cb += "01";                    // input count
    cb += std::string(64, '0');    // prev txid (null)
    cb += "ffffffff";              // prev vout
    char lb[3]; std::snprintf(lb, sizeof(lb), "%02zx", ss_len);
    cb += lb;                      // scriptSig length
    cb += script_sig;
    cb += "ffffffff";              // sequence
    char ob[3]; std::snprintf(ob, sizeof(ob), "%02x", outputs);
    cb += ob;                      // output count
    auto append_output = [&](std::int64_t v, std::string_view spk) {
        cb += utils::le_hex_u64(static_cast<std::uint64_t>(v));
        std::size_t n = spk.size() / 2;
        char l[3]; std::snprintf(l, sizeof(l), "%02zx", n);
        cb += l; cb += spk;
    };
    append_output(miner_value, miner_script);
    if (donation > 0 && !donation_script.empty()) append_output(donation, donation_script);
    cb += "00000000";             // locktime
    return cb;
}

std::uint32_t now_unix() {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// After submitblock, nudge our own node to prefer our block in a same-height
// race. Without it, if the node saw a competitor block first, it keeps that
// competitor as tip and our own hashrate helps orphan our own block. Failure
// is harmless (first-seen rules still apply), so errors are logged at info
// and never propagate.
void mark_block_precious(const std::shared_ptr<bitcoin::BitcoinClient>& btc,
                         const std::string& block_hash_hex) {
    if (!btc) return;
    btc->asyncPreciousBlock(block_hash_hex,
        [block_hash_hex](const boost::system::error_code& ec, const nlohmann::json& rsp) {
            if (ec) {
                spdlog::info("[Session] preciousblock transport error hash={}: {}",
                             block_hash_hex, ec.message());
                return;
            }
            if (rsp.contains("error") && !rsp["error"].is_null()) {
                spdlog::info("[Session] preciousblock not applied hash={}: {}",
                             block_hash_hex, rsp["error"].dump());
            } else {
                spdlog::info("[Session] preciousblock applied hash={}", block_hash_hex);
            }
        });
}

bool parse_hex_u32(std::string_view h, std::uint32_t& out) {
    if (h.size() != 8) return false;
    try {
        out = static_cast<std::uint32_t>(std::stoul(std::string(h), nullptr, 16));
    } catch (...) { return false; }
    return true;
}

// JSON-RPC 2.0 allows `id` to be null, a number, or a string. nlohmann's
// `msg.value("id", 0)` THROWS type_error on a string id, which under 50K
// miners is a guaranteed crash given how loose real-world Stratum clients are.
// This helper returns the id pre-rendered as a JSON literal ("null", "42",
// or "\"abc\"") that can be spliced straight into our fmt::format reply.
std::string safe_id_json(const nlohmann::json& msg) {
    auto it = msg.find("id");
    if (it == msg.end() || it->is_null()) return "null";
    try {
        if (it->is_number_integer())   return fmt::format("{}", it->get<std::int64_t>());
        if (it->is_number_unsigned())  return fmt::format("{}", it->get<std::uint64_t>());
        if (it->is_number_float())     return fmt::format("{}", it->get<double>());
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

} // anonymous

// ---------------------------------------------------------------------------

ClientSession::ClientSession(boost::asio::io_context& io,
                             const CoinConfig& coin,
                             StratifierPtr strat,
                             std::shared_ptr<bitcoin::BitcoinClient> btc,
                             std::shared_ptr<RateLimiter> rl,
                             std::shared_ptr<bitcoin::BitcoinClient> auxBtc,
                             std::shared_ptr<boost::asio::ssl::context> tls_ctx)
    : io_(io),
      strand_(boost::asio::make_strand(io.get_executor())),
      tls_ctx_(std::move(tls_ctx)),
      is_tls_(tls_ctx_ != nullptr),
      socket_(io, tls_ctx_.get()),
      writes_(socket_),
      coin_(coin),
      strat_(std::move(strat)),
      btc_(std::move(btc)),
      rl_(std::move(rl)),
      auxBtc_(std::move(auxBtc)),
      // Bracket the vardiff bounds around the tier's starting difficulty so a
      // fixed tier whose difficulty lies outside the default [vardiffMin,
      // vardiffMax] band is not silently clamped (VarDiff clamps the initial
      // difficulty). e.g. fixed-50M (>10M default max) was served as 10M, and
      // ZEC fixed-2M (256 powLimit < 1024 default min) as 1024. For real
      // vardiff tiers the start already sits inside the band, so this is a
      // no-op. Fixed tiers never retarget (gated on vardiffEnabled), so the
      // widened band cannot cause drift.
      vardiff_({.target_share_rate_hz   = coin.targetSharesPerMinute / 60.0,
                .min_difficulty         = std::min(coin.activeTier.vardiffMin,
                                                   coin.activeTier.startingDifficulty),
                .max_difficulty         = std::max(coin.activeTier.vardiffMax,
                                                   coin.activeTier.startingDifficulty),
                .tau_seconds            = coin.vardiffTauSeconds},
               coin.activeTier.startingDifficulty),
      last_difficulty_(coin.activeTier.startingDifficulty),
      last_difficulty_change_ts_(std::chrono::steady_clock::now()) {
    extranonce1_ = utils::generate_extranonce1();
    session_id_  = utils::generate_session_id();
    if (coin.chain == ChainKind::Zcash) {
        extranonce2_size_ = 4;
    }
    writes_.set_drop_handler([this]{ on_disconnect_local("write watermark"); });
}

void ClientSession::start() {
    // Install a self-keeper into the write queue so chained async_write
    // handlers hold a shared_ptr ref to this session for their lifetime.
    // This prevents heap-use-after-free if the session is dropped while
    // a write is still in flight.
    writes_.set_owner(std::weak_ptr<void>(shared_from_this()));
    // Serialise the outbound write chain on this session's strand so it can
    // never race with the strand-bound socket close in on_disconnect_local().
    writes_.set_executor(strand_);
    connected_at_ = std::chrono::steady_clock::now();
    connected_unix_ = static_cast<std::int64_t>(now_unix());
    boost::system::error_code ec;
    // Disable Nagle on the ACCEPTED socket. tcp::no_delay set on the acceptor
    // (connector.cpp) does NOT propagate to accepted sockets on Linux, so
    // without this every stratum response is subject to Nagle + delayed-ACK
    // (~40ms stalls), which throttles submit->ack latency and throughput.
    socket_.lowest_layer().set_option(boost::asio::ip::tcp::no_delay(true), ec);
    auto ep = socket_.lowest_layer().remote_endpoint(ec);
    client_ip_ = ec ? std::string{"?"} : ep.address().to_string();
    metrics::inc_connections_accepted();
    spdlog::debug("[Session {}] connected", client_ip_);

    auto self = shared_from_this();

    // TLS tiers terminate the TLS handshake first, then run plain Stratum V1
    // (encrypted on the wire). TLS is never enabled on the exclusive V2 port,
    // so there is no V1-vs-V2 auto-detection here: a TLS connection is V1.
    if (is_tls_) {
        socket_.async_handshake(
            boost::asio::ssl::stream_base::server,
            boost::asio::bind_executor(strand_,
                [self](const boost::system::error_code& hec) {
                    if (hec) {
                        self->on_disconnect_local("TLS handshake failed");
                        return;
                    }
                    spdlog::debug("[Session {}] TLS handshake complete", self->client_ip_);
                    self->is_v2_ = false;
                    self->do_read();
                }));
        return;
    }

    // Read 1 byte to auto-detect V1 vs V2, or read 66 bytes for Noise Act 1.
    // Detected as exclusive-V2 when the tier sets the sv2 flag (new, allows many
    // SV2 tiers with distinct labels) OR carries the legacy "stratumv2" label
    // (pre-existing fixed/vardiff SV2 tiers that predate the flag).
    bool is_exclusive_v2 = self->coin_.activeTier.sv2
                           || self->coin_.activeTier.label == "stratumv2";
    
    if (is_exclusive_v2 && self->coin_.sv2NoiseRequired) {
        spdlog::info("[SV2 Session {}] Strict V2 Noise NX handshake (no V1 fallback)...", client_ip_);
        boost::asio::async_read(socket_, boost::asio::buffer(read_buf_.data(), 1),
            boost::asio::bind_executor(strand_, [self](const boost::system::error_code& fec, std::size_t fbytes) {
                if (fec || fbytes != 1) {
                    self->on_disconnect_local("V2 port first byte read failed");
                    return;
                }
                
                uint8_t first_byte = static_cast<uint8_t>(self->read_buf_[0]);
                spdlog::info("[SV2 Session {}] First byte: 0x{:02x}", self->client_ip_, first_byte);
                
                if (first_byte == '{' || std::isspace(first_byte)) {
                    spdlog::warn("[SV2 Session {}] V1 connection rejected on exclusive V2 Noise port", self->client_ip_);
                    self->on_disconnect_local("V1 connection rejected on exclusive V2 port");
                    return;
                }
                
                // Binary data = Noise Act 1 (64 bytes total, we have 1)
                spdlog::info("[SV2 Session {}] Binary data, reading remaining 63 bytes of Noise Act 1", self->client_ip_);
                boost::asio::async_read(self->socket_, boost::asio::buffer(self->read_buf_.data() + 1, 63),
                    boost::asio::bind_executor(self->strand_, [self](const boost::system::error_code& aec, std::size_t abytes) {
                        if (aec || abytes != 63) {
                            self->on_disconnect_local("Noise Act 1 remaining read failed");
                            return;
                        }
                        
                        if (!self->noise_state_.initialize(self->coin_.sv2AuthorityKey)) {
                            self->on_disconnect_local("Failed to initialize NoiseState with authority key");
                            return;
                        }
                        
                        if (!self->noise_state_.process_act1(reinterpret_cast<const uint8_t*>(self->read_buf_.data()))) {
                            self->on_disconnect_local("Failed to process Noise Act 1");
                            return;
                        }
                        
                        std::vector<uint8_t> act2;
                        try {
                            act2 = self->noise_state_.generate_act2();
                        } catch (const std::exception& ex) {
                            spdlog::error("[SV2 Session {}] Act 2 generation error: {}", self->client_ip_, ex.what());
                            self->on_disconnect_local("Failed to generate Noise Act 2");
                            return;
                        }
                        
                        spdlog::info("[SV2 Session {}] Sending Noise Act 2 ({} bytes)", self->client_ip_, act2.size());
                        auto act2_shared = std::make_shared<std::vector<uint8_t>>(std::move(act2));
                        boost::asio::async_write(self->socket_, boost::asio::buffer(act2_shared->data(), act2_shared->size()),
                            boost::asio::bind_executor(self->strand_, [self, act2_shared](const boost::system::error_code& wec, std::size_t) {
                                if (wec) {
                                    self->on_disconnect_local("Failed to write Noise Act 2");
                                    return;
                                }
                                self->is_v2_ = true;
                                self->is_v2_encrypted_ = true;
                                self->v2_handshake_done_ = true;
                                spdlog::info("[SV2 Session {}] Noise handshake complete, entered encrypted transport", self->client_ip_);
                                self->do_read_v2(22); // Read 22-byte encrypted V2 header
                            })
                        );
                    })
                );
            })
        );
    } else {
        boost::asio::async_read(socket_, boost::asio::buffer(read_buf_.data(), 1),
            boost::asio::bind_executor(strand_, [self, is_exclusive_v2](const boost::system::error_code& pec, std::size_t pbytes) {
                if (pec || pbytes != 1) {
                    self->on_disconnect_local("handshake error");
                    return;
                }
                uint8_t first_byte = static_cast<uint8_t>(self->read_buf_[0]);
                if (first_byte == '{' || std::isspace(first_byte)) {
                    // Stratum V1
                    if (is_exclusive_v2) {
                        self->on_disconnect_local("V1 connection rejected on exclusive V2 port");
                        return;
                    }
                    self->is_v2_ = false;
                    self->buffer_.push_back(first_byte); // Push back the first byte
                    self->do_read(); // Continue with standard V1 line reading
                } else {
                    // Stratum V2!
                    if (!is_exclusive_v2) {
                        self->on_disconnect_local("non-V1 connection rejected on exclusive V1 port");
                        return;
                    }
                    self->is_v2_ = true;
                    self->sv2_read_buffer_.push_back(first_byte);
                    self->do_read_v2(5); // Read the rest of the 6-byte header
                }
            })
        );
    }
}

void ClientSession::shutdown() {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self]{ self->on_disconnect_local("shutdown"); });
}

void ClientSession::on_disconnect_local(const char* reason) {
    if (closed_.exchange(true)) return;
    auto lifetime_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - connected_at_).count();
    spdlog::info("[Session {}] disconnect: {} (worker={} lifetime={}s accepted={} rejected={})",
                 client_ip_,
                 reason,
                 worker_name_.empty() ? "<none>" : worker_name_,
                 lifetime_s,
                 shares_accepted_.load(std::memory_order_relaxed),
                 shares_rejected_.load(std::memory_order_relaxed));
    // Stop the write queue before closing the socket so no chained async_write
    // is issued on a descriptor we are about to free.
    writes_.stop();
    boost::system::error_code ec;
    // Hard TCP close on the lowest layer. For TLS we deliberately skip the
    // async SSL_shutdown handshake (a truncation is harmless here and avoids
    // keeping the session alive purely to exchange close_notify).
    socket_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    socket_.lowest_layer().close(ec);
    if (!worker_address_.empty()) {
        db_worker().enqueue_presence({worker_address_, worker_name_, "offline", client_ip_});
    }
    if (rl_) rl_->on_disconnect(client_ip_);
    if (disconnect_handler_) disconnect_handler_(shared_from_this());
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

void ClientSession::do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(read_buf_.data(), read_buf_.size()),
        boost::asio::bind_executor(strand_,
            [self](const boost::system::error_code& ec, std::size_t n) {
                self->on_read(ec, n);
            }));
}

void ClientSession::on_read(const boost::system::error_code& ec, std::size_t n) {
    if (ec) { on_disconnect_local(ec.message().c_str()); return; }
    buffer_.append(read_buf_.data(), n);
    // Cap buffer to defeat slowloris-style memory attacks.
    if (buffer_.size() > (1u << 20)) {
        on_disconnect_local("oversize buffer");
        return;
    }
    while (true) {
        auto pos = buffer_.find('\n');
        if (pos == std::string::npos) break;
        std::string_view line(buffer_.data(), pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        process_line(line);
        buffer_.erase(0, pos + 1);
    }
    if (!closed_.load()) do_read();
}

void ClientSession::process_line(std::string_view line) {
    if (line.empty()) return;
    nlohmann::json msg;
    try {
        msg = nlohmann::json::parse(line);
    } catch (...) {
        send_line(R"({"id":null,"result":null,"error":[20,"JSON parse error",null]})" "\n");
        return;
    }
    if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string()) {
        send_line(R"({"id":null,"result":null,"error":[20,"missing method",null]})" "\n");
        return;
    }
    // Hardening: any nlohmann type_error (e.g. malformed params, unexpected
    // id type) must NOT propagate -- boost::asio rethrows from handlers and
    // a single bad frame would crash the whole pool with 50K miners attached.
    try {
        const std::string method = msg["method"].get<std::string>();
        spdlog::info("[Session {}] <- {}", client_ip_, method);
        using namespace stratum;
        if      (method == kMethodConfigure)         handle_configure(msg);
        else if (method == kMethodSubscribe)         handle_subscribe(msg);
        else if (method == kMethodAuthorize)         handle_authorize(msg);
        else if (method == kMethodSubmit)            handle_submit(msg);
        else if (method == kMethodSuggestDifficulty) handle_suggest_difficulty(msg);
        else if (method == kMethodExtranonceSub) {
            extranonce_subscribed_ = true;
            send_line(fmt::format(R"({{"id":{},"result":true,"error":null}})" "\n",
                                  safe_id_json(msg)));
            if (auto j = strat_->latestJob()) {
                emit_job(*j, true);
            }
        } else {
            // Sanitize the method name (it survived JSON parse but may contain
            // backslash/quote that would corrupt our error frame).
            std::string safe;
            safe.reserve(method.size());
            for (char c : method) {
                if (c == '"' || c == '\\' || (unsigned char)c < 0x20) safe.push_back('?');
                else safe.push_back(c);
            }
            send_line(fmt::format(
                R"({{"id":{},"result":null,"error":[20,"Unknown method: {}",null]}})" "\n",
                safe_id_json(msg), safe));
        }
    } catch (const std::exception& e) {
        spdlog::warn("[Session {}] handler exception: {}", client_ip_, e.what());
        send_line(fmt::format(
            R"({{"id":{},"result":null,"error":[20,"malformed request",null]}})" "\n",
            safe_id_json(msg)));
    } catch (...) {
        spdlog::warn("[Session {}] handler unknown exception", client_ip_);
        send_line(R"({"id":null,"result":null,"error":[20,"internal error",null]})" "\n");
    }
}

void ClientSession::send_line(std::string msg) {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, m = std::move(msg)]() mutable {
        self->writes_.push(std::move(m));
    });
}

// Emit a Stratum V1 client.reconnect. Posted to the strand for thread
// safety; V2 sessions are skipped (the protocol has no such notification).
void ClientSession::requestReconnect(std::string host, std::string port, int wait) {
    auto self = shared_from_this();
    boost::asio::post(strand_,
        [self, host = std::move(host), port = std::move(port), wait]() mutable {
            if (self->closed_.load() || self->is_v2_) return;
            nlohmann::json params = nlohmann::json::array();
            if (!host.empty()) {
                params.push_back(host);
                // port as a number when possible (miners expect numeric), else string.
                try { params.push_back(std::stoi(port)); }
                catch (...) { params.push_back(port); }
                params.push_back(wait > 0 ? wait : 0);
            }
            nlohmann::json note = {
                {"id", nullptr}, {"method", "client.reconnect"}, {"params", params}};
            spdlog::info("[Session {}] -> client.reconnect {}", self->client_ip_, note["params"].dump());
            self->writes_.push(note.dump() + "\n");
        });
}

// Rebuild and publish the control-plane snapshot. Runs on the strand.
void ClientSession::publish_snapshot() {
    auto s = std::make_shared<StatSnapshot>();
    s->ip              = client_ip_;
    s->worker_address  = worker_address_;
    s->worker_name     = worker_name_;
    s->user_agent      = user_agent_;
    s->protocol        = protocol_label();
    s->miner_id        = miner_id_;
    s->connected_unix  = connected_unix_;
    s->tier_port       = coin_.activeTier.port;
    s->authorized      = authorized_;
    s->subscribed      = subscribed_;
    s->is_v2           = is_v2_;
    s->is_tls          = is_tls_;
    stat_snap_.store(std::shared_ptr<const StatSnapshot>(std::move(s)));
}

void ClientSession::resetShareCounters() {
    auto self = shared_from_this();
    boost::asio::post(strand_, [self] {
        self->shares_total_ = 0;
        self->shares_accepted_.store(0, std::memory_order_relaxed);
        self->shares_rejected_.store(0, std::memory_order_relaxed);
        // Numerics are live (read on demand); identity is unchanged, so there is
        // nothing to republish here.
    });
}

// Shares->hashes multiplier for this chain (see header).
double ClientSession::hashrate_multiplier() const noexcept {
    switch (coin_.chain) {
        case ChainKind::Litecoin:
        case ChainKind::Dogecoin: return 65536.0;        // Scrypt (2^16)
        case ChainKind::Zcash:    return 8192.0;         // Equihash (matches diff-1 scoring)
        default:                  return 4294967296.0;   // SHA-256d (2^32)
    }
}

// Called on every accepted share from the session strand. Records the last
// share time for idle detection and folds the difficulty into the pool's
// best-share-of-round. Deliberately does NOT publish the snapshot (that stays at
// job cadence) so the hot path adds only a timestamp store + one atomic max.
void ClientSession::note_accepted_share(double share_diff) {
    last_share_unix_.store(static_cast<std::int64_t>(now_unix()), std::memory_order_relaxed);
    // Refresh the live hashrate estimate from vardiff's already-maintained
    // decayed share-rate (dsps) - a couple of relaxed stores on this session's
    // own cache line, no allocation and no cross-session contention.
    const double mult = hashrate_multiplier();
    stat_hr1_.store(vardiff_.dsps1() * mult, std::memory_order_relaxed);
    stat_hr5_.store(vardiff_.dsps5() * mult, std::memory_order_relaxed);
    if (strat_) strat_->note_share_diff(share_diff);
}

// ---------------------------------------------------------------------------
// BIP310 mining.configure
// ---------------------------------------------------------------------------
void ClientSession::handle_configure(const nlohmann::json& msg) {
    const auto id_json = safe_id_json(msg);
    auto params = nlohmann::json::array();
    if (auto it = msg.find("params"); it != msg.end() && it->is_array()) {
        params = *it;
    }
    auto neg = stratum::negotiate_configure(params,
        coin_.enableVersionRolling ? strat_->pool_version_mask() : 0);
    version_rolling_ = neg.version_rolling && neg.version_mask != 0;
    version_mask_    = neg.version_mask;

    // Build the BIP310 reply directly via fmt (no DOM). Per spec the result must
    // echo ONLY the features the miner actually requested: strict ASIC firmware
    // (e.g. Antminer S21 XP) treats a response carrying results for features it
    // never asked for as malformed and silently refuses to hash. Result values
    // are booleans / mask-hex; nothing here needs JSON escaping.
    std::string body;
    auto append = [&body](std::string_view kv) {
        if (!body.empty()) body.push_back(',');
        body.append(kv);
    };
    if (neg.version_rolling_requested) {
        if (neg.version_rolling) {
            append(fmt::format(R"("version-rolling":true,"version-rolling.mask":"{:08x}")",
                               neg.version_mask));
        } else {
            append(R"("version-rolling":false)");
        }
    }
    if (neg.minimum_difficulty)   append(R"("minimum-difficulty":true)");
    if (neg.subscribe_extranonce) append(R"("subscribe-extranonce":true)");

    send_line(fmt::format(
        R"({{"id":{},"result":{{{}}},"error":null}})" "\n", id_json, body));
}

// ---------------------------------------------------------------------------
// mining.subscribe
// ---------------------------------------------------------------------------
namespace {
// Aggregator detection. NiceHash multiplexes many miners over a single
// stratum connection and submits at extreme rates (one share per ~10ms),
// which swamps low-diff tiers. We only allow NiceHash on tiers that opt
// in via allowNiceHash=true (the high-diff ports).
//
// NOTE: MiningRigRentals is *not* matched here. MRR is a marketplace, not
// an aggregator: each rented rig is its own TCP connection at its own real
// hashrate (Bitaxe / S19 / etc.) and forwards the downstream miner's UA.
// MRR rigs are free to use any tier appropriate for their hashrate.
bool looks_like_aggregator(std::string_view ua) {
    if (ua.empty()) return false;
    std::string lower(ua);
    for (auto& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
    static constexpr std::string_view needles[] = {
        "nicehash",
    };
    for (auto n : needles) if (lower.find(n) != std::string::npos) return true;
    return false;
}
} // anonymous

void ClientSession::handle_subscribe(const nlohmann::json& msg) {
    const auto id_json = safe_id_json(msg);

    // Capture miner-supplied user-agent (Stratum V1: params[0] is a string
    // like "NiceHash/1.0.0" or "Antminer S19k Pro/Tue Apr 9 ...").
    if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty()
        && msg["params"][0].is_string()) {
        user_agent_ = msg["params"][0].get<std::string>();
        if (user_agent_.size() > 256) user_agent_.resize(256);
    }

    // Aggregator gate: reject UA-identified NiceHash/MRR on tiers that don't
    // opt in. Send a stratum error and tear down the connection so the
    // upstream proxy reroutes to a permitted port.
    if (!coin_.activeTier.allowNiceHash && looks_like_aggregator(user_agent_)) {
        spdlog::warn("[Session {}] reject UA='{}' on tier :{} ({}): aggregator not allowed here",
                     client_ip_, user_agent_,
                     coin_.activeTier.port,
                     coin_.activeTier.label.empty() ? "unnamed" : coin_.activeTier.label);
        send_line(fmt::format(
            R"({{"id":{},"result":null,"error":[20,"aggregator services must connect to a high-diff tier port",null]}})" "\n",
            id_json));
        on_disconnect_local("ua-gated");
        return;
    }

    extranonce2_size_ = (coin_.chain == ChainKind::Zcash) ? 4 : 8;
    send_line(fmt::format(
        R"({{"id":{},"result":[[["mining.set_difficulty","{}"],["mining.notify","{}"]],"{}",{}],"error":null}})" "\n",
        id_json, session_id_, session_id_, extranonce1_, extranonce2_size_));

    subscribed_ = true;
    publish_snapshot();  // make the connection visible in `clients` immediately
    spdlog::info("[Session {}] subscribed UA='{}' tier=:{}/{}",
                 client_ip_, user_agent_,
                 coin_.activeTier.port,
                 coin_.activeTier.label.empty() ? "unnamed" : coin_.activeTier.label);

    // Initial difficulty + (optional) version-mask broadcast.
    send_set_difficulty(vardiff_.current());
    if (version_rolling_ && version_mask_) send_set_version_mask(version_mask_);
}

void ClientSession::send_set_difficulty(double d, std::optional<double> old_diff) {
    double rounded = std::round(d);
    // Track previous difficulty for pipeline transition grace window
    last_difficulty_ = old_diff.value_or(d);
    last_difficulty_change_ts_ = std::chrono::steady_clock::now();
    stat_diff_.store(rounded, std::memory_order_relaxed);  // control-plane display

    spdlog::info("[Session {}] -> mining.set_difficulty {:.0f} (raw={:.3f})", client_ip_, rounded, d);
    send_line(fmt::format(
        R"({{"id":null,"method":"mining.set_difficulty","params":[{:.0f}]}})" "\n", rounded));

    // Zcash/Equihash miners (NiceHash, MRR, EWBF, lolMiner, GMiner) act on an
    // explicit 256-bit target via mining.set_target and ignore the Bitcoin-style
    // set_difficulty scalar above. Send the target as well; this is additive (the
    // scalar is kept for difficulty-aware clients) and gated to Zcash only, so no
    // other chain's behaviour changes.
    if (coin_.chain == ChainKind::Zcash) {
        // diff-1 target = 524287 * 2^224 - identical to the basis used when scoring
        // submitted shares (see handle_submit), so a solution meeting this target
        // scores share_diff >= the advertised difficulty.
        long double max_target = 524287.0L * std::pow(2.0L, 224.0L);
        long double tval = (rounded > 0.0) ? (max_target / static_cast<long double>(rounded)) : max_target;
        std::array<uint8_t, 32> tgt_be{};   // big-endian: [0] = most-significant byte
        long double cur = tval;
        for (int i = 31; i >= 0; --i) {
            tgt_be[i] = static_cast<uint8_t>(std::fmod(cur, 256.0L));
            cur = std::floor(cur / 256.0L);
        }
        std::string tgt_hex;
        tgt_hex.reserve(64);
        for (uint8_t b : tgt_be) tgt_hex += fmt::format("{:02x}", b);
        spdlog::info("[Session {}] -> mining.set_target {} (diff={:.0f})", client_ip_, tgt_hex, rounded);
        send_line(fmt::format(
            R"({{"id":null,"method":"mining.set_target","params":["{}"]}})" "\n", tgt_hex));
    }

    // Immediately push the latest job to force the miner to adopt the new difficulty
    if (authorized_) {
        if (auto j = strat_->latestJob()) {
            emit_job(*j, true);
        }
    }
}

void ClientSession::send_set_version_mask(std::uint32_t m) {
    send_line(fmt::format(
        R"({{"id":null,"method":"mining.set_version_mask","params":["{:08x}"]}})" "\n", m));
}

// ---------------------------------------------------------------------------
// mining.authorize (local address validation)
// ---------------------------------------------------------------------------
void ClientSession::handle_authorize(const nlohmann::json& msg) {
    const auto id_json = safe_id_json(msg);

    // Tiny JSON-string escape for error text that may pass through (address
    // decoder strings are constant, but we belt-and-brace here).
    auto escape_json_err = [](std::string_view s) {
        std::string o; o.reserve(s.size());
        for (char c : s) {
            if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
            else if ((unsigned char)c < 0x20) o.push_back('?');
            else o.push_back(c);
        }
        return o;
    };

    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()
        || !msg["params"][0].is_string()) {
        send_line(fmt::format(
            R"({{"id":{},"result":false,"error":"missing worker name"}})" "\n", id_json));
        return;
    }
    const std::string full = msg["params"][0].get<std::string>();
    auto dot = full.find('.');
    std::string addr   = (dot == std::string::npos) ? full : full.substr(0, dot);
    std::string worker = (dot == std::string::npos) ? std::string{"0"} : full.substr(dot + 1);
    if (addr.size() > 128 || worker.size() > 64) {
        send_line(fmt::format(
            R"({{"id":{},"result":false,"error":"name too long"}})" "\n", id_json));
        return;
    }

    auto decoded = address::decode(addr);
    if (!decoded) {
        spdlog::info("[Session {}] reject auth (addr={}): {}", client_ip_, addr,
                     address::error_to_string(decoded.error()));
        send_line(fmt::format(
            R"({{"id":{},"result":false,"error":"invalid address: {}"}})" "\n",
            id_json, escape_json_err(address::error_to_string(decoded.error()))));
        return;
    }

    // ----- Blacklist / reserved-name enforcement -----
    // All in-memory (no DB on this hot path); refreshed on a timer + SIGHUP.
    // Done before the node validateaddress RPC so rejects are cheap.
    {
        const auto& bl = Blacklist::instance();
        if (bl.address_blocked(addr)) {
            spdlog::warn("[Session {}] reject auth: address blacklisted (addr={})", client_ip_, addr);
            send_line(fmt::format(
                R"({{"id":{},"result":false,"error":"address not allowed"}})" "\n", id_json));
            return;
        }
        if (bl.worker_is_free(worker)) {
            // "FREE-HASHRATE" is reserved: only addresses we've granted may use it.
            if (!bl.free_granted(addr)) {
                spdlog::warn("[Session {}] reject auth: reserved worker name without grant (addr={} worker={})",
                             client_ip_, addr, worker);
                send_line(fmt::format(
                    R"({{"id":{},"result":false,"error":"worker name is reserved"}})" "\n", id_json));
                return;
            }
        } else if (bl.worker_blocked(worker)) {
            spdlog::warn("[Session {}] reject auth: worker name blacklisted (addr={} worker={})",
                         client_ip_, addr, worker);
            send_line(fmt::format(
                R"({{"id":{},"result":false,"error":"worker name not allowed"}})" "\n", id_json));
            return;
        }
    }

    double parsed_custom_diff = 0.0;
    bool has_custom_diff = false;

    // Process custom difficulty from password (only on vardiff-enabled ports)
    if (coin_.activeTier.vardiffEnabled && msg["params"].size() >= 2 && msg["params"][1].is_string()) {
        std::string password = msg["params"][1].get<std::string>();
        std::string pwd_lower = password;
        std::transform(pwd_lower.begin(), pwd_lower.end(), pwd_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        
        std::size_t pos = pwd_lower.find("diff=");
        if (pos != std::string::npos) {
            pos += 5;
        } else {
            pos = pwd_lower.find("d=");
            if (pos != std::string::npos) {
                pos += 2;
            }
        }
        
        if (pos != std::string::npos) {
            try {
                std::string num_str;
                while (pos < pwd_lower.size() && (std::isdigit(pwd_lower[pos]) || pwd_lower[pos] == '.' || pwd_lower[pos] == 'k' || pwd_lower[pos] == 'm' || pwd_lower[pos] == 'g' || pwd_lower[pos] == 't')) {
                    num_str.push_back(pwd_lower[pos]);
                    pos++;
                }
                if (!num_str.empty()) {
                    double val = 0.0;
                    double multiplier = 1.0;
                    if (num_str.back() == 'k') {
                        multiplier = 1000.0;
                        num_str.pop_back();
                    } else if (num_str.back() == 'm') {
                        multiplier = 1000000.0;
                        num_str.pop_back();
                    } else if (num_str.back() == 'g') {
                        multiplier = 1000000000.0;
                        num_str.pop_back();
                    } else if (num_str.back() == 't') {
                        multiplier = 1000000000000.0;
                        num_str.pop_back();
                    }
                    val = std::stod(num_str) * multiplier;
                    if (std::isfinite(val) && val > 0.0) {
                        if (coin_.chain == ChainKind::Zcash) {
                            // ZEC difficulty is advertised in standard (2^256) units to
                            // match miners/MRR/2miners, but the pool scores internally in
                            // powLimit units (1/8192 of standard). Accept the standard
                            // number the renter typed and convert; clamp on the standard
                            // value so the documented bounds read naturally. Other chains
                            // already use their native stratum units here.
                            parsed_custom_diff = std::clamp(val, 8192.0, 524288.0) / 8192.0;
                        } else if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin) {
                            // Scrypt (LTC/DOGE) left on the legacy range pending testing.
                            parsed_custom_diff = std::clamp(val, 1024.0, 10000000.0);
                        } else {
                            // SHA256 coins (BTC/BCH/BC2/BCH2/DGB/XEC): custom diff is capped
                            // at the vardiff max (1,000,000).
                            parsed_custom_diff = std::clamp(val, 1024.0, 1000000.0);
                        }
                        has_custom_diff = true;
                    }
                }
            } catch (...) {}
        }
    }

    std::string aux_doge_address;
    if (msg["params"].size() >= 2 && msg["params"][1].is_string()) {
        std::string password = msg["params"][1].get<std::string>();
        std::string pwd_lower = password;
        std::transform(pwd_lower.begin(), pwd_lower.end(), pwd_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        
        std::size_t doge_pos = pwd_lower.find("doge=");
        if (doge_pos != std::string::npos) {
            std::size_t start = doge_pos + 5;
            // Tolerate whitespace after '=' (e.g. "DOGE= addr"): skip leading blanks
            // so a stray space no longer parses to an empty address (LTC-only).
            while (start < password.size() &&
                   (password[start] == ' ' || password[start] == '\t')) ++start;
            std::size_t end = password.find_first_of(",;\t ", start);
            aux_doge_address = (end == std::string::npos)
                ? password.substr(start)
                : password.substr(start, end - start);
        }
    }

    // Perform node-level validation check via validateaddress to guarantee 100% network correctness
    auto self = shared_from_this();
    btc_->asyncValidateAddress(addr, [self, id_json, addr, worker, decoded, parsed_custom_diff, has_custom_diff, aux_doge_address](const boost::system::error_code& ec, bool valid, bool script, bool segwit) {
        boost::asio::post(self->strand_, [self, id_json, addr, worker, decoded, parsed_custom_diff, has_custom_diff, aux_doge_address, ec, valid, script, segwit]() {
            if (self->closed_.load()) return;

            auto js_escape = [](std::string_view s) {
                std::string o; o.reserve(s.size());
                for (char c : s) {
                    if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
                    else if ((unsigned char)c < 0x20) o.push_back('?');
                    else o.push_back(c);
                }
                return o;
            };

            if (ec || !valid) {
                std::string err_msg = ec ? ec.message() : "node rejected address as invalid or unsupported";
                spdlog::info("[Session {}] reject auth (addr={}) via node-level validation: {}", self->client_ip_, addr, err_msg);
                self->send_line(fmt::format(
                    R"({{"id":{},"result":false,"error":"invalid address: {}"}})" "\n",
                    id_json, js_escape(err_msg)));
                return;
            }

            auto proceed_auth = [self, id_json, addr, worker, decoded, parsed_custom_diff, has_custom_diff]() {
                self->worker_address_    = addr;
                self->worker_name_       = std::move(worker);
                if (self->coin_.chain == ChainKind::Zcash) {
                    // Zcash transparent addresses use 2-byte version prefixes that
                    // address::decode doesn't handle; derive the script directly so
                    // we can redirect the coinbase miner output to this worker.
                    auto zs = zcash::taddr_to_script(addr);
                    self->worker_script_hex_ = zs.empty()
                        ? std::string{} : utils::bytes_to_hex({zs.data(), zs.size()});
                } else {
                    self->worker_script_hex_ = address::script_pubkey_hex(*decoded);
                }

                if (has_custom_diff) {
                    self->custom_difficulty_ = parsed_custom_diff;
                    self->has_custom_difficulty_ = true;
                    self->vardiff_.set_current(self->custom_difficulty_);
                    self->send_set_difficulty(self->custom_difficulty_);
                }

                self->send_line(fmt::format(
                    R"({{"id":{},"result":true,"error":null}})" "\n", id_json));

                self->miner_id_ = Database::ensure_miner(
                    self->worker_address_, self->worker_name_, self->client_ip_);
                if (self->miner_id_ < 0) {
                    spdlog::error("[Session {}] miner id allocation failed", self->client_ip_);
                }

                db_worker().enqueue_presence({self->worker_address_, self->worker_name_, "online", self->client_ip_});
                self->authorized_ = true;
                self->publish_snapshot();  // initial control-plane snapshot
                spdlog::info("[Session {}] authorized worker={} addr={} (node validated)",
                             self->client_ip_, self->worker_name_, self->worker_address_);

                // Send latest job (if any).
                if (auto j = self->strat_->latestJob()) {
                    self->emit_job(*j);
                } else {
                    spdlog::warn("[Session {}] no job available at authorize time", self->client_ip_);
                }
            };

            if (!aux_doge_address.empty() && self->auxBtc_) {
                self->auxBtc_->asyncValidateAddress(aux_doge_address, [self, id_json, proceed_auth, aux_doge_address](const boost::system::error_code& ec_doge, bool valid_doge, bool script_doge, bool segwit_doge) {
                    boost::asio::post(self->strand_, [self, id_json, proceed_auth, aux_doge_address, ec_doge, valid_doge]() {
                        if (self->closed_.load()) return;
                        if (ec_doge || !valid_doge) {
                            spdlog::info("[Session {}] reject auth (invalid DOGE address={})", self->client_ip_, aux_doge_address);
                            self->send_line(fmt::format(
                                R"({{"id":{},"result":false,"error":"invalid Dogecoin address"}})" "\n", id_json));
                            return;
                        }
                        self->aux_doge_address_ = aux_doge_address;
                        if (auto d = address::decode(aux_doge_address)) {
                            self->doge_script_hex_ = address::script_pubkey_hex(*d);
                        } else {
                            spdlog::warn("[Session {}] DOGE address validated by node but local decode failed: {}",
                                         self->client_ip_, aux_doge_address);
                        }
                        spdlog::info("[Session {}] validated custom DOGE payout address={}", self->client_ip_, aux_doge_address);
                        proceed_auth();
                    });
                });
            } else {
                // No miner-supplied DOGE address: fall back to the pool's default
                // DOGE payout address (coin.aux.payoutAddress) so LTC hashrate still
                // merge-mines DOGE and we never silently miss DOGE blocks. Blocks
                // found this way pay the pool's configured DOGE address.
                if (self->auxBtc_ && !self->coin_.aux.payoutAddress.empty()) {
                    if (auto d = address::decode(self->coin_.aux.payoutAddress)) {
                        self->aux_doge_address_ = self->coin_.aux.payoutAddress;
                        self->doge_script_hex_  = address::script_pubkey_hex(*d);
                        spdlog::info("[Session {}] no miner DOGE address; merge-mining DOGE to pool default {}",
                                     self->client_ip_, self->coin_.aux.payoutAddress);
                    } else {
                        spdlog::warn("[Session {}] pool default DOGE address failed local decode: {}",
                                     self->client_ip_, self->coin_.aux.payoutAddress);
                    }
                }
                proceed_auth();
            }
        });
    });
}

// ---------------------------------------------------------------------------
// mining.suggest_difficulty
// ---------------------------------------------------------------------------
void ClientSession::handle_suggest_difficulty(const nlohmann::json& msg) {
    const auto id_json = safe_id_json(msg);
    // In fixed-diff mode we ignore miner-suggested difficulty: the tier
    // port already encodes the operator's intended diff and honouring a
    // suggestion would let any miner unilaterally undershoot it. Still ACK
    // so well-behaved clients don't error.
    if (coin_.activeTier.vardiffEnabled
        && msg.contains("params") && msg["params"].is_array() && !msg["params"].empty()
        && msg["params"][0].is_number()) {
        double d = msg["params"][0].get<double>();
        if (std::isfinite(d)) {
            d = std::clamp(d, coin_.activeTier.vardiffMin, coin_.activeTier.vardiffMax);
            send_set_difficulty(d);
        }
    }
    send_line(fmt::format(
        R"({{"id":{},"result":true,"error":null}})" "\n", id_json));
}

// Build self-assembled DOGE merge-mining work for this session+job. Returns
// nullopt when merge mining is not applicable (no DOGE address, aux disabled,
// or the template lacks aux fields). Synchronous: no RPC, no node aux cache.
std::optional<ClientSession::SessionAuxJob>
ClientSession::build_session_aux(const MiningJob& job) const {
    if (!job.aux_enabled || doge_script_hex_.empty()) return std::nullopt;
    if (job.aux_prevhash.size() != 64 || job.aux_bits.size() != 8) return std::nullopt;
    if (job.aux_target.size() != 64) return std::nullopt;

    std::string donation_script;
    if (coin_.aux.donationPercent > 0.0 && !coin_.aux.donationAddress.empty()) {
        if (auto d = address::decode(coin_.aux.donationAddress))
            donation_script = address::script_pubkey_hex(*d);
    }

    std::string doge_cb = build_doge_coinbase(
        job.aux_height, coin_.coinbaseSignature, job.aux_coinbasevalue,
        doge_script_hex_, donation_script, coin_.aux.donationPercent);
    if (doge_cb.empty()) return std::nullopt;

    // Coinbase-only DOGE block: merkle root == coinbase txid (internal order).
    std::string merkleroot = utils::compute_coinbase_txid_le(doge_cb);
    std::string doge_header =
          utils::le_hex_u32(job.aux_version)
        + utils::byte_reverse_hex(job.aux_prevhash)   // display -> internal
        + merkleroot
        + utils::le_hex_u32(job.aux_curtime)
        + utils::byte_reverse_hex(job.aux_bits)       // display -> internal
        + "00000000";                                 // nonce (PoW comes from parent)
    // Aux block identity hash. Dogecoin's CheckAuxPow reverses the chain merkle
    // root to display (big-endian) order before searching the parent coinbase,
    // so the commitment must carry the byte-reversed hash, not the raw internal
    // bytes - otherwise: "Aux POW missing chain merkle root in parent coinbase".
    std::string aux_hash = utils::byte_reverse_hex(utils::compute_coinbase_txid_le(doge_header));

    SessionAuxJob s;
    s.aux_target     = job.aux_target;
    s.aux_commit     = "fabe6d6d" + aux_hash + "01000000" + "00000000";
    s.doge_coinbase  = std::move(doge_cb);
    s.doge_header    = std::move(doge_header);
    s.doge_block_hash = aux_hash;  // already display order
    s.aux_height     = job.aux_height;
    s.aux_value      = job.aux_coinbasevalue;
    return s;
}

// ---------------------------------------------------------------------------
// mining.notify (broadcast hook)
// ---------------------------------------------------------------------------
void ClientSession::notifyNewJob(JobPtr job) {
    if (!job) return;
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, job]() mutable {
        if (!self->subscribed_ || self->closed_.load()) return;
        // NB: no snapshot publish here - this runs for every session on every
        // job broadcast. Identity is published at subscribe/authorize; the live
        // numerics are atomics read on demand. Keeps 50k workers cheap per block.

        if (job->aux_enabled && !self->aux_doge_address_.empty() && self->auxBtc_) {
            if (auto aux = self->build_session_aux(*job)) {
                // Bound the per-job map; jobs roll over quickly on a fast aux tip.
                if (self->session_aux_jobs_.size() > 256) self->session_aux_jobs_.clear();
                self->session_aux_jobs_[job->job_id] = *aux;

                std::string session_coinbase1 = job->coinbase1;
                if (session_coinbase1.size() >= 88) {
                    session_coinbase1.replace(session_coinbase1.size() - 88, 88, aux->aux_commit);
                }

                std::string prevhash_stratum;
                prevhash_stratum.resize(64);
                if (job->previousblockhash.size() == 64) {
                    for (std::size_t w = 0; w < 8; ++w) {
                        std::memcpy(&prevhash_stratum[w * 8],
                                    &job->previousblockhash[(7 - w) * 8], 8);
                    }
                } else {
                    prevhash_stratum = job->previousblockhash;
                }

                // NOTE: prefix must end with `","` so emit_job_custom's appended
                // coinbase2 hex lands as a quoted JSON string (the suffix supplies
                // the closing quote). Ending with just `",` produced an unquoted
                // coinb2 -> malformed mining.notify -> miners drop the connection.
                std::string custom_prefix = fmt::format(
                    R"({{"id":null,"method":"mining.notify","params":["{}","{}","{}",")",
                    job->job_id, prevhash_stratum, session_coinbase1);

                self->emit_job_custom(*job, custom_prefix);
                return;
            }
        }

        if (self->is_v2_) {
            self->emit_job_v2(*job);
        } else {
            self->emit_job(*job);
        }
    });
}

void ClientSession::emit_job(const MiningJob& job, bool force_clean) {
    std::string out;

    if (coin_.chain == ChainKind::Zcash) {
        // Default: the node-built coinbase (pays the node's address).
        out = job.notify_prefix;
        // Per-miner direct payout: redirect the coinbase miner-reward output to
        // this worker's t-address (funding streams preserved), recompute the
        // ZIP-244 coinbase txid and the merkle root, and rebuild the notify.
        if (!worker_script_hex_.empty() && !job.zcash_full_coinbase_hex.empty()) {
            zcash::Bytes don_script;
            if (job.donation_percent > 0.0 && !job.donation_address.empty())
                don_script = zcash::taddr_to_script(job.donation_address);
            auto swapped = zcash::swap_miner_output(
                utils::hex_to_bytes(job.zcash_full_coinbase_hex),
                utils::hex_to_bytes(worker_script_hex_),
                don_script, job.donation_percent);
            if (!swapped.empty()) {
                auto cb_txid = zcash::zip244_txid(swapped);
                std::string cb_txid_hex = utils::bytes_to_hex({cb_txid.data(), cb_txid.size()});
                std::string mr = merkle::compute_root_le_hex(cb_txid_hex, job.merkle_branch_le);
                if (zcash_jobs_.size() > 256) zcash_jobs_.clear();
                zcash_jobs_[job.job_id] = {
                    utils::bytes_to_hex({swapped.data(), swapped.size()}), mr };

                std::string version_le      = utils::le_hex_u32(job.version);
                std::string prevhash_stratum = utils::byte_reverse_hex(job.previousblockhash);
                std::string reserved_hex     = job.reserved.empty()
                    ? std::string(64, '0') : utils::byte_reverse_hex(job.reserved);
                std::string time_le = utils::le_hex_u32(job.curtime);
                std::string bits_le = utils::byte_reverse_hex(job.bits);
                out = fmt::format(
                    R"({{"id":null,"method":"mining.notify","params":["{}","{}","{}","{}","{}","{}","{}",{}]}})" "\n",
                    job.job_id, version_le, prevhash_stratum, mr, reserved_hex,
                    time_le, bits_le, job.clean_jobs ? "true" : "false");
            }
        }
    } else {
        // Bitcoin-family: coinbase2 is per-session in solo mode (miner-specific payout output).
        std::string cb2 = build_session_coinbase2(job, worker_script_hex_);

        // Hot fan-out path: O(1) memcpy chain. All JSON-escaping concerns are
        // resolved at job-creation time on the Stratifier (job_id / prevhash /
        // cb1 / merkle branch / version / bits / curtime / clean). cb2 here is
        // pure ASCII hex built by build_session_coinbase2 -> no escaping needed.
        out.reserve(job.notify_prefix.size() + cb2.size() + job.notify_suffix.size());
        out.append(job.notify_prefix);
        out.append(cb2);
        out.append(job.notify_suffix);
    }

    bool is_clean = job.clean_jobs || force_clean;
    if (force_clean && out.size() >= 9 && out.compare(out.size() - 9, 9, ",false]}\n") == 0) {
        out.replace(out.size() - 9, 9, ",true]}\n");
    }

    spdlog::info("[Session {}] -> mining.notify job={} clean={} (forced={}) branch_n={}",
                 client_ip_, job.job_id, is_clean, force_clean, job.merkle_branch_le.size());
    // Use send_line() so this enqueue runs through the same async strand
    // path as subscribe-result / set_difficulty / authorize-result. Pushing
    // directly here introduces an ordering race when subscribe+authorize
    // arrive in one TCP segment: the notify would land before the
    // subscribe-result, leaving the rig without an extranonce1 to combine
    // with the coinbase, so it discards the job and goes idle.
    send_line(std::move(out));
}

void ClientSession::emit_job_custom(const MiningJob& job, const std::string& custom_prefix, bool force_clean) {
    std::string out;
    std::string cb2 = build_session_coinbase2(job, worker_script_hex_);

    out.reserve(custom_prefix.size() + cb2.size() + job.notify_suffix.size());
    out.append(custom_prefix);
    out.append(cb2);
    out.append(job.notify_suffix);

    bool is_clean = job.clean_jobs || force_clean;
    if (force_clean && out.size() >= 9 && out.compare(out.size() - 9, 9, ",false]}\n") == 0) {
        out.replace(out.size() - 9, 9, ",true]}\n");
    }

    spdlog::info("[Session {}] -> mining.notify (custom aux) job={} clean={} (forced={}) branch_n={}",
                 client_ip_, job.job_id, is_clean, force_clean, job.merkle_branch_le.size());
    send_line(std::move(out));
}

std::string ClientSession::build_session_coinbase2(
    const MiningJob& job, std::string_view miner_script_hex) const {
    if (miner_script_hex.empty()) return {};

    std::string cb2;
    cb2.reserve(160 + 80);
    cb2 += "ffffffff"; // sequence

    std::int64_t total = job.coinbase_value;
    std::int64_t donation = 0;
    std::string donation_script;

    std::int64_t miner_fund_val = 0;
    std::string miner_fund_script = "a914d37c4c809fe9840e7bfa77b86bd47163f6fb6c6087"; // fallback P2SH script

    std::int64_t staking_reward_val = 0;
    std::string staking_reward_script = "76a9140e1ce96692cf15155985bf010264725428d3233e88ac"; // fallback P2PKH script
    
    if (coin_.chain == ChainKind::eCash) {
        // Use GBT values if provided, otherwise compute standard 32% / 10% payouts
        if (job.miner_fund_value > 0) {
            miner_fund_val = job.miner_fund_value;
        } else {
            miner_fund_val = static_cast<std::int64_t>(std::llround((double)total * 32.0 / 100.0));
        }

        if (!job.miner_fund_address.empty()) {
            auto decoded = address::decode(job.miner_fund_address);
            if (decoded) {
                miner_fund_script = address::script_pubkey_hex(*decoded);
            }
        }

        if (job.staking_rewards_value > 0) {
            staking_reward_val = job.staking_rewards_value;
        } else {
            staking_reward_val = static_cast<std::int64_t>(std::llround((double)total * 10.0 / 100.0));
        }

        if (!job.staking_rewards_script.empty()) {
            staking_reward_script = job.staking_rewards_script;
        }
    }

    std::int64_t remaining_pool_value = total - miner_fund_val - staking_reward_val;

    if (job.donation_percent > 0.0 && !job.donation_address.empty()) {
        auto don_decoded = address::decode(job.donation_address);
        if (don_decoded) {
            donation = static_cast<std::int64_t>(
                std::llround((double)remaining_pool_value * job.donation_percent / 100.0));
            if (donation > remaining_pool_value) {
                donation = 0;
            } else {
                donation_script = address::script_pubkey_hex(*don_decoded);
            }
        } else {
            spdlog::warn("[Session] donation address invalid: {}", job.donation_address);
        }
    }
    std::int64_t miner_value = remaining_pool_value - donation;

    bool add_commit = job.segwit_active && !job.witness_commitment.empty();
    
    int outputs = 0;
    if (coin_.chain == ChainKind::eCash) {
        outputs = (donation > 0 ? 2 : 1) + 2; // miner + donation + miner_fund + staking_rewards
    } else {
        outputs = (donation > 0 ? 2 : 1) + (add_commit ? 1 : 0);
    }

    char ocbuf[3];
    std::snprintf(ocbuf, sizeof(ocbuf), "%02x", outputs);
    cb2 += ocbuf;

    auto append_output = [&](std::int64_t value, std::string_view spk_hex) {
        cb2 += utils::le_hex_u64(static_cast<std::uint64_t>(value));
        std::size_t spk_bytes = spk_hex.size() / 2;
        char lb[3]; std::snprintf(lb, sizeof(lb), "%02zx", spk_bytes);
        cb2 += lb;
        cb2 += spk_hex;
    };

    append_output(miner_value, miner_script_hex);
    if (donation > 0 && !donation_script.empty()) {
        append_output(donation, donation_script);
    }
    if (coin_.chain == ChainKind::eCash) {
        append_output(miner_fund_val, miner_fund_script);
        append_output(staking_reward_val, staking_reward_script);
    }
    if (add_commit) {
        cb2 += utils::le_hex_u64(0);
        std::size_t cl = job.witness_commitment.size() / 2;
        char lb[3]; std::snprintf(lb, sizeof(lb), "%02zx", cl);
        cb2 += lb;
        cb2 += job.witness_commitment;
    }
    cb2 += "00000000"; // locktime
    return cb2;
}

// ---------------------------------------------------------------------------
// mining.submit
// ---------------------------------------------------------------------------
void ClientSession::handle_submit(const nlohmann::json& msg) {
    auto t0 = std::chrono::steady_clock::now();
    const auto id_json = safe_id_json(msg);

    // `code` is the Stratum V1 rejection code:
    //   20 Other/Unknown, 21 Job not found (=stale), 22 Duplicate share,
    //   23 Low difficulty share, 24 Unauthorized worker, 25 Not subscribed.
    // Defaults to 20 for the malformed-submit cases (bad params/hex/size);
    // call sites pass the specific code where one applies.
    auto reject = [&](const char* reason, const char* metric_reason, int code = 20) {
        spdlog::warn("[Session {}] share rejected ({}): {}", client_ip_, metric_reason, reason);
        // `reason` is always a string literal from the call sites below; no
        // JSON escaping required.
        send_line(fmt::format(
            R"({{"id":{},"result":null,"error":[{},"{}",null]}})" "\n",
            id_json, code, reason));
        shares_rejected_.fetch_add(1, std::memory_order_relaxed);
        metrics::inc_share_rejected(metric_reason);
        if (rl_ && rl_->record_invalid_share(client_ip_)) {
            on_disconnect_local("ban: too many invalid shares");
        }
    };

    if (!authorized_) { reject("not authorized", "unauthorized", 24); return; }
    if (!msg.contains("params") || !msg["params"].is_array() ||
        msg["params"].size() < 5) {
        reject("bad params", "bad-params"); return;
    }
    auto p = msg["params"];
    std::string worker, jobId, nonce2, ntime_s, nonce_s, solution_s;
    std::optional<std::uint32_t> version_bits;

    if (coin_.chain == ChainKind::Zcash) {
        // Zcash submit: [worker, job_id, ntime, nonce2, solution]
        // nonce2 = portion of 32-byte header nonce after extranonce1
        if (p.size() < 5) { reject("bad params", "bad-params"); return; }
        try {
            worker     = p[0].get<std::string>();
            jobId      = p[1].get<std::string>();
            ntime_s    = p[2].get<std::string>();
            nonce_s    = p[3].get<std::string>();  // this is the nonce2 portion
            solution_s = p[4].get<std::string>();
        } catch (...) { reject("bad params types", "bad-params"); return; }

        if (!utils::valid_hex(ntime_s) || ntime_s.size() != 8) {
            reject("invalid time hex", "bad-hex"); return;
        }
        if (!utils::valid_hex(nonce_s)) {
            reject("invalid nonce hex", "bad-hex"); return;
        }
        // Reconstruct the full 32-byte nonce = extranonce1 + nonce2
        // nonce_s here is the miner's nonce2 portion (after en1)
        std::string full_nonce = extranonce1_ + nonce_s;
        // Pad to exactly 64 hex chars (32 bytes) if needed
        while (full_nonce.size() < 64) full_nonce.push_back('0');
        if (full_nonce.size() != 64) {
            reject("bad Zcash nonce size", "bad-nonce-size"); return;
        }
        nonce_s = full_nonce;  // replace with full nonce for header construction

        if (!utils::valid_hex(solution_s)) {
            reject("bad Zcash solution hex", "bad-solution-size"); return;
        }
        // nheqminer serializes solution with compactSize prefix (fd4005 for 1344 bytes = 6 hex chars)
        if (solution_s.size() == 2694) {
            // Strip the 3-byte compactSize prefix (fd4005)
            solution_s = solution_s.substr(6);
        }
        if (solution_s.size() != 2688) {
            spdlog::warn("[Session {}] bad Zcash solution size: {} hex chars", client_ip_, solution_s.size());
            reject("bad Zcash solution size", "bad-solution-size"); return;
        }
    } else {
        // Bitcoin-family submit: [worker, job_id, extranonce2, ntime, nonce, (version_bits)]
        try {
            worker  = p[0].get<std::string>();
            jobId   = p[1].get<std::string>();
            nonce2  = p[2].get<std::string>();
            ntime_s = p[3].get<std::string>();
            nonce_s = p[4].get<std::string>();
            if (p.size() >= 6 && p[5].is_string()) {
                std::uint32_t v;
                if (parse_hex_u32(p[5].get<std::string>(), v)) version_bits = v;
            }
        } catch (...) { reject("bad params types", "bad-params"); return; }

        if (!utils::valid_hex(nonce2) || !utils::valid_hex(ntime_s) ||
            !utils::valid_hex(nonce_s) || ntime_s.size() != 8) {
            reject("invalid hex", "bad-hex"); return;
        }
        if (nonce_s.size() != 8) {
            reject("bad nonce size", "bad-nonce-size"); return;
        }
    }

    // extranonce2 size check (only for non-Zcash; Zcash has no en2 in coinbase).
    if (coin_.chain != ChainKind::Zcash) {
        const std::size_t kEn2HexChars = extranonce2_size_ * 2;
        if (nonce2.size() != kEn2HexChars) {
            spdlog::warn("[Session {}] bad en2 size: got {} hex chars, want {} (rig en2_size mismatch?)",
                         client_ip_, nonce2.size(), kEn2HexChars);
            reject("bad extranonce2 size", "bad-en2-size"); return;
        }
    }

    // Address & worker match.
    auto dot = worker.find('.');
    std::string w_addr   = (dot == std::string::npos) ? worker : worker.substr(0, dot);
    std::string w_name   = (dot == std::string::npos) ? std::string{"0"} : worker.substr(dot + 1);
    if (w_addr != worker_address_ || w_name != worker_name_) {
        reject("address/worker mismatch", "bad-worker"); return;
    }

    // Job lookup via rolling window.
    auto job = strat_->findJob(jobId);
    if (!job) {
        spdlog::warn("[Session {}] job not found: {}", client_ip_, jobId);
        reject("job not found", "stale", 21);
        metrics::inc_share_stale();
        return;
    }

    // Stale-by-block detection. A share whose job was built on a now-superseded
    // prevhash must NOT be counted toward round effort (counting it is the root
    // cause of runaway effort%) and must NOT be ACKed as accepted. But it is
    // still hashed and tested against its job's own network target further
    // down, so a solve is never discarded untested: right after a block change
    // a stale solve is a same-height chain race, not a guaranteed orphan, and
    // the node adjudicates. The reject itself therefore happens after the hash
    // is computed, and still WITHOUT touching the invalid-share ban counter,
    // since a block change legitimately produces a burst of these from
    // in-flight miners (especially proxies that switch slowly). Empty tip =
    // pre-first-job; treat as not-stale to be safe.
    const std::string cur_prev = strat_->current_prevhash();
    const bool stale_by_block =
        !cur_prev.empty() && job->previousblockhash != cur_prev;

    // ntime sanity (BIP113 / consensus).
    if (coin_.chain == ChainKind::Zcash) {
        ntime_s = utils::byte_reverse_hex(ntime_s);
    }
    std::uint32_t submitted_ntime;
    if (!parse_hex_u32(ntime_s, submitted_ntime)) { reject("bad ntime", "bad-hex"); return; }
    if (!utils::valid_ntime(submitted_ntime, job->mintime, now_unix())) {
        reject("ntime out of range", "ntime"); return;
    }

    // Version-rolling (not used for Zcash but harmless to compute).
    std::uint32_t effective_mask = version_mask_;
    if (effective_mask == 0 && coin_.enableVersionRolling) {
        effective_mask = strat_->pool_version_mask();
    }
    std::uint32_t version = job->version;
    if (version_bits) {
        if (effective_mask == 0) { reject("version-rolling not negotiated", "vrb"); return; }
        version = (job->version & ~effective_mask) | (*version_bits & effective_mask);
    }
    std::string version_hex_le = utils::le_hex_u32(version);

    // Rebuild coinbase + merkle root + header.
    std::string merkle_root_le;
    std::string cb_hex;  // needed for block assembly on a win
    if (coin_.chain == ChainKind::Zcash) {
        // Use the per-miner redirected coinbase + recomputed merkle root that we
        // sent in mining.notify; fall back to the node's coinbase if absent.
        auto zit = zcash_jobs_.find(jobId);
        if (zit != zcash_jobs_.end()) {
            cb_hex = zit->second.coinbase_hex;
            merkle_root_le = zit->second.merkle_root_le;
        } else {
            merkle_root_le = job->zcash_merkle_root_le;  // already LE from stratifier
            cb_hex = job->zcash_full_coinbase_hex;
        }
    } else {
        std::string cb2_session = build_session_coinbase2(*job, worker_script_hex_);
        std::string full_extra = extranonce1_ + nonce2;
        std::string active_coinbase1 = job->coinbase1;
        if (!aux_doge_address_.empty()) {
            auto it = session_aux_jobs_.find(jobId);
            if (it != session_aux_jobs_.end() && active_coinbase1.size() >= 88) {
                // Splice the exact same merged-mining commitment that was sent in
                // mining.notify, so the reconstructed coinbase (hence merkle root)
                // matches what the miner hashed.
                active_coinbase1.replace(active_coinbase1.size() - 88, 88, it->second.aux_commit);
            }
        }
        cb_hex = utils::rebuild_coinbase(active_coinbase1, cb2_session, full_extra);
        std::string cb_txid_le = utils::compute_coinbase_txid_le(cb_hex);
        merkle_root_le = merkle::compute_root_le_hex(cb_txid_le, job->merkle_branch_le);
    }

    std::string header_hex;
    std::vector<std::uint8_t> header_bytes;
    if (coin_.chain == ChainKind::Zcash) {
        // Zcash block header is 140 bytes
        std::string res_hex = job->reserved.empty() ? std::string(64, '0') : job->reserved;
        header_hex.reserve(280);
        header_hex.append(version_hex_le)
                  .append(utils::byte_reverse_hex(job->previousblockhash))
                  .append(merkle_root_le)
                  .append(utils::byte_reverse_hex(res_hex))
                  .append(utils::byte_reverse_hex(ntime_s))
                  .append(utils::byte_reverse_hex(job->bits))
                  .append(nonce_s); // Already LE from miner's CDataStream serialization
        header_bytes = utils::hex_to_bytes(header_hex);
    } else {
        // Standard 80-byte header
        header_hex = utils::build_block_header(
            version_hex_le,
            utils::byte_reverse_hex(job->previousblockhash),  // BE displayed -> LE bytes
            merkle_root_le,
            utils::byte_reverse_hex(ntime_s),                 // stratum BE -> LE
            utils::byte_reverse_hex(job->bits),               // stratum BE -> LE
            utils::byte_reverse_hex(nonce_s));                // stratum BE -> LE
        header_bytes = utils::hex_to_bytes(header_hex);
    }

    std::array<std::uint8_t, 32> sha_out{};
    if (coin_.chain == ChainKind::Zcash) {
        auto solution_bytes = utils::hex_to_bytes(solution_s);
        if (!equihash::verify_200_9(header_bytes, solution_bytes)) {
            spdlog::warn("[Session {}] bad equihash verification: header_hex={} solution_s={}",
                         client_ip_, header_hex, solution_s);
            reject("invalid equihash solution", "bad-equihash"); return;
        }
        // Hash for target check is double-SHA256 of completed header (140 bytes + 3 bytes compactSize + 1344 solution bytes)
        std::vector<std::uint8_t> full_zcash_hdr;
        full_zcash_hdr.reserve(header_bytes.size() + 3 + solution_bytes.size());
        full_zcash_hdr.insert(full_zcash_hdr.end(), header_bytes.begin(), header_bytes.end());
        full_zcash_hdr.push_back(0xfd);
        full_zcash_hdr.push_back(0x40);
        full_zcash_hdr.push_back(0x05);
        full_zcash_hdr.insert(full_zcash_hdr.end(), solution_bytes.begin(), solution_bytes.end());
        sha_out = utils::sha256d_arr({full_zcash_hdr.data(), full_zcash_hdr.size()});
    } else if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin) {
        sha_out = utils::scrypt_1024_1_1_32({header_bytes.data(), header_bytes.size()});
    } else {
        sha_out = utils::sha256d_arr({header_bytes.data(), header_bytes.size()});
    }

    // SHA-256 output for Bitcoin/Zcash is the integer in LITTLE-ENDIAN byte order
    // Reverse here so [0]=MSB, matching the target encoding.
    std::array<std::uint8_t, 32> header_hash_be{};
    for (std::size_t i = 0; i < 32; ++i) header_hash_be[i] = sha_out[31 - i];

    // Convert hash to integer (big-endian = displayed reverse).
    auto net_target_bytes = utils::target_from_bits_hex(job->bits);
    // Compare big-endian: hash_be < net_target_be?
    auto le_byte_cmp = [](const auto& a, const auto& b) {
        // a and b are 32-byte big-endian. Return <0 if a<b, 0 if eq, >0 if a>b.
        for (std::size_t i = 0; i < 32; ++i) {
            if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        }
        return 0;
    };
    bool meets_network = le_byte_cmp(header_hash_be, net_target_bytes) <= 0;

    // Approx share difficulty (precise enough for vardiff and accounting).
    double share_diff = 0.0;
    {
        long double hash_as_d = 0.0L, base = 1.0L;
        for (int i = 31; i >= 0; --i) {
            hash_as_d += (long double)header_hash_be[i] * base;
            base *= 256.0L;
        }
        long double max_target_d;
        if (coin_.chain == ChainKind::Zcash) {
            max_target_d = (long double)524287.0L * std::pow(2.0L, 224.0L);
        } else if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin) {
            max_target_d = (long double)0xFFFF * std::pow(2.0L, 224.0L);
        } else {
            max_target_d = (long double)0xFFFF * std::pow(2.0L, 208.0L);
        }
        if (hash_as_d > 0.0L) share_diff = (double)(max_target_d / hash_as_d);
    }

    // Stale shares: test-and-submit any block solve, then reject the share
    // exactly as before (same error, same metrics, no ban bump, no effort).
    // Submission is gated on first-sighting via the duplicate table so a proxy
    // replaying the same in-flight share cannot double-submit.
    if (stale_by_block) {
        const std::string stale_hash_hex =
            utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()});
        if (strat_->register_share(stale_hash_hex)) {
            if (meets_network) {
                spdlog::warn("[Session {}] STALE share solves block! worker={} height={} share_diff={:.4f} - submitting anyway (chain race)",
                             client_ip_, worker, job->height, share_diff);
                submit_block_candidate(job, worker, header_hex, cb_hex,
                                       solution_s, stale_hash_hex, share_diff);
            }
            // A stale *parent* can still win the current DOGE block: auxpow
            // does not require the parent header to be a valid LTC block.
            maybe_submit_aux_block(job, jobId, worker, header_hex, cb_hex,
                                   header_hash_be, share_diff);
        }
        spdlog::info("[Session {}] stale share: job prevhash={}... tip={}... (block changed)",
                     client_ip_, job->previousblockhash.substr(0, 12), cur_prev.substr(0, 12));
        send_line(fmt::format(
            R"({{"id":{},"result":null,"error":[21,"stale share - job for previous block",null]}})" "\n",
            id_json));
        shares_rejected_.fetch_add(1, std::memory_order_relaxed);
        metrics::inc_share_stale();
        return;
    }

    double target_diff = vardiff_.current();
    bool grace_applied = false;
    double grace_age = 0.0;
    const double tolerance = (coin_.chain == ChainKind::Zcash) ? 0.90 : 0.99;

    if (share_diff < target_diff) {
        grace_age = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_difficulty_change_ts_).count();
        if (grace_age < 40.0 && share_diff >= last_difficulty_ * tolerance) {
            target_diff = last_difficulty_;
            grace_applied = true;
        }
    }

    if (share_diff < target_diff * tolerance) {
        // Always submit any possible block solve: on low-diff chains/testnets
        // the network target can sit below the vardiff floor, so a share
        // rejected for difficulty may still be a valid block.
        if (meets_network) {
            spdlog::warn("[Session {}] low-diff share solves block! worker={} height={} share_diff={:.4f} - submitting anyway",
                         client_ip_, worker, job->height, share_diff);
            submit_block_candidate(
                job, worker, header_hex, cb_hex, solution_s,
                utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()}),
                share_diff);
        }
        spdlog::info("[Session {}] low-diff share_diff={:.3f} vardiff={:.3f} (last_diff={:.3f}, age={:.1f}s)",
                     client_ip_, share_diff, vardiff_.current(), last_difficulty_, grace_age);
        reject("low difficulty share", "low-diff", 23);
        return;
    }

    if (grace_applied) {
        spdlog::info("[Session {}] grace-accepted share_diff={:.3f} under old difficulty {:.3f} (new target was {:.3f}, age={:.1f}s)",
                     client_ip_, share_diff, last_difficulty_, vardiff_.current(), grace_age);
    }

    // Duplicate-share gate. The header hash is the
    // deterministic fingerprint of (job, enonce1, nonce2, ntime, nonce,
    // version), so a repeat is a replay/proxy resubmission and must not be
    // counted again - double-counting pads round effort exactly like stale
    // shares do. The table is global (Stratifier) and purged each block. A
    // duplicate's first occurrence already handled any block submission, so we
    // return here without re-submitting. No ban-counter bump: a flapping proxy
    // behind a NAT must not get the whole IP banned.
    const std::string share_hash_hex =
        utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()});
    if (!strat_->register_share(share_hash_hex)) {
        spdlog::info("[Session {}] duplicate share hash={} job={}", client_ip_, share_hash_hex, jobId);
        send_line(fmt::format(
            R"({{"id":{},"result":null,"error":[22,"duplicate share",null]}})" "\n", id_json));
        shares_rejected_.fetch_add(1, std::memory_order_relaxed);
        metrics::inc_share_rejected("duplicate");
        return;
    }

    send_line(fmt::format(
        R"({{"id":{},"result":true,"error":null}})" "\n", id_json));
    shares_accepted_.fetch_add(1, std::memory_order_relaxed);
    note_accepted_share(share_diff);   // idle + best-share
    if (rl_) rl_->clear_invalid(client_ip_);
    metrics::inc_share_accepted();

    // Vardiff tick (only on tiers that opted in). On fixed-diff tiers the
    // accept threshold stays frozen at activeTier.startingDifficulty for
    // the whole session.
    if (coin_.activeTier.vardiffEnabled && !has_custom_difficulty_) {
        // Network difficulty from the job's compact bits. Caps retargets
        // so we never demand more work than a block, which is the
        // runaway-fix on low-diff chains like testnet.
        double network_diff = 0.0;
        {
            long double net_d = 0.0L, base = 1.0L;
            for (int i = 31; i >= 0; --i) {
                net_d += (long double)net_target_bytes[i] * base;
                base *= 256.0L;
            }
            long double max_t;
            if (coin_.chain == ChainKind::Zcash) {
                max_t = (long double)524287.0L * std::pow(2.0L, 224.0L);
            } else if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin) {
                max_t = (long double)0xFFFF * std::pow(2.0L, 224.0L);
            } else {
                max_t = (long double)0xFFFF * std::pow(2.0L, 208.0L);
            }
            if (net_d > 0.0L) network_diff = (double)(max_t / net_d);
        }
        double old_diff = vardiff_.current();
        if (auto nd = vardiff_.on_share(std::chrono::steady_clock::now(),
                                        target_diff, network_diff)) {
            send_set_difficulty(*nd, old_diff);
        }
    }

    // Persist (async).
    if (miner_id_ > 0) {
        db_worker().enqueue_share(ShareRecord{
            miner_id_, jobId, nonce_s, extranonce1_, nonce2,
            share_hash_hex,
            target_diff, true, meets_network});
        db_worker().enqueue_best_share(BestShareRecord{
            miner_id_, share_diff, share_hash_hex});
    }

    // ----- DOGE merge mining (self-assembled aux block) ---------------------
    maybe_submit_aux_block(job, jobId, worker, header_hex, cb_hex,
                           header_hash_be, share_diff);

    if (meets_network) {
        submit_block_candidate(job, worker, header_hex, cb_hex, solution_s,
                               share_hash_hex, share_diff);
    }

    auto us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count();
    metrics::observe_share_processing_us(us);
}

void ClientSession::maybe_submit_aux_block(const JobPtr& job, const std::string& jobId,
                                           const std::string& worker,
                                           const std::string& header_hex,
                                           const std::string& cb_hex,
                                           const std::array<std::uint8_t, 32>& header_hash_be,
                                           double share_diff) {
    const SessionAuxJob* saj = nullptr;
    if (job->aux_enabled && !aux_doge_address_.empty()) {
        auto it = session_aux_jobs_.find(jobId);
        if (it != session_aux_jobs_.end()) saj = &it->second;
    }
    if (!saj || saj->aux_target.size() != 64 || !auxBtc_) return;

    // DOGE getblocktemplate "target" is big-endian (display) order - compare
    // directly against the big-endian parent PoW hash, no reversal.
    std::array<std::uint8_t, 32> aux_target_bytes{};
    auto tbytes = utils::hex_to_bytes(saj->aux_target);
    if (tbytes.size() == 32) std::copy(tbytes.begin(), tbytes.end(), aux_target_bytes.begin());
    bool meets_aux = true;
    for (std::size_t i = 0; i < 32; ++i) {
        if (header_hash_be[i] != aux_target_bytes[i]) {
            meets_aux = header_hash_be[i] < aux_target_bytes[i];
            break;
        }
    }
    if (!meets_aux) return;

    // Dedup: every share on a winning job solves the same aux block; submit it
    // to dogecoind only once (subsequent shares would just return "duplicate").
    if (doge_submitted_.count(saj->doge_block_hash)) return;
    if (doge_submitted_.size() > 512) doge_submitted_.clear();
    doge_submitted_.insert(saj->doge_block_hash);
    spdlog::info("[Session {}] DOGE AUX BLOCK CANDIDATE worker={} height={} hash={} share_diff={:.4f}",
                 client_ip_, worker, saj->aux_height, saj->doge_block_hash, share_diff);

    // AuxPoW = parent coinbase ‖ parent block hash(32) ‖ coinbase merkle
    // branch ‖ nIndex(0) ‖ chain merkle branch(empty) ‖ nChainIndex(0) ‖
    // parent header(80). cb_hex is the legacy (non-segwit) LTC coinbase.
    std::string parent_hash = utils::compute_coinbase_txid_le(header_hex); // sha256d(header), internal
    std::string merkle_branch_hex = fmt::format("{:02x}", job->merkle_branch_le.size());
    for (const auto& h : job->merkle_branch_le) merkle_branch_hex += h;

    std::string auxpow = cb_hex
                       + parent_hash          // CMerkleTx.hashBlock (32)
                       + merkle_branch_hex     // coinbase branch: count + hashes
                       + "00000000"            // nIndex = 0 (coinbase)
                       + "00"                  // chain merkle branch count = 0
                       + "00000000"            // nChainIndex = 0
                       + header_hex;           // parent (LTC) header (80)

    // Full DOGE block = header(80, auxpow-flag version, nonce 0) ‖ auxpow ‖
    // txcount(1) ‖ coinbase. Submitted via DOGE submitblock - no node aux
    // cache involved, so "block hash unknown" can never occur.
    std::string doge_block = saj->doge_header + auxpow + "01" + saj->doge_coinbase;

    const int          aux_miner_id = miner_id_;
    const double       aux_diff     = share_diff;
    const std::int64_t aux_reward   = saj->aux_value;
    auxBtc_->asyncSubmitBlock(doge_block,
        [worker, height = saj->aux_height, bh = saj->doge_block_hash,
         aux_miner_id, aux_diff, aux_reward](const boost::system::error_code& ec, const nlohmann::json& rsp) {
            if (ec) {
                spdlog::error("[Session] DOGE submitblock RPC error worker={} h={} hash={}: {}",
                              worker, height, bh, ec.message());
                return;
            }
            if (rsp.contains("error") && !rsp["error"].is_null()) {
                spdlog::error("[Session] DOGE block rejected worker={} h={} hash={} error={}",
                              worker, height, bh, rsp["error"].dump());
                return;
            }
            // submitblock returns null on accept, or a string reason
            // ("duplicate", "inconclusive", "high-hash", ...).
            if (rsp.contains("result") && rsp["result"].is_string()) {
                spdlog::warn("[Session] DOGE submitblock worker={} h={} hash={} result={}",
                             worker, height, bh, rsp["result"].get<std::string>());
                return;
            }
            spdlog::info("[Session] DOGE BLOCK ACCEPTED worker={} h={} hash={}", worker, height, bh);
            metrics::inc_block_found();
            // Persist the DOGE block too (was previously never recorded). Tagged
            // coin="DOGE" so it is PURELY ADDITIVE: db_worker does not let it
            // reset/snapshot the shared (LTC) effort accumulator. height is the
            // DOGE chain height; hash is the DOGE block hash.
            db_worker().enqueue_block(BlockRecord{
                .height       = static_cast<std::int64_t>(height),
                .block_hash   = bh,
                .miner_id     = aux_miner_id,
                .difficulty   = aux_diff,
                .reward_value = aux_reward,
                .coin         = "DOGE"});
        });
}

void ClientSession::submit_block_candidate(const JobPtr& job, const std::string& worker,
                                           const std::string& header_hex,
                                           const std::string& cb_hex,
                                           const std::string& solution_s,
                                           const std::string& block_hash_hex,
                                           double share_diff) {
    spdlog::info("[Session {}] BLOCK CANDIDATE worker={} height={} share_diff={:.4f}",
                 client_ip_, worker, job->height, share_diff);

    // eCash Real-Time Target (RTT) guard: a block found too soon after its
    // parent is rejected by avalanche (policy-bad-rtt) and orphaned. Detect that
    // here and withhold it rather than submit a guaranteed orphan; the share is
    // already credited upstream. XEC-only and requires GBT rtt data - a no-op
    // for every other chain (rtt_present is never set off eCash templates).
    if (coin_.chain == ChainKind::eCash && job->rtt_present) {
        const auto hb = utils::hex_to_bytes(block_hash_hex);
        if (hb.size() == 32) {
            std::array<std::uint8_t, 32> hash_be{};
            for (std::size_t i = 0; i < 32; ++i) hash_be[i] = hb[i];
            if (!utils::ecash_rtt_ok(job->rtt_prevheadertime, job->rtt_prevbits,
                                     static_cast<std::int64_t>(now_unix()), hash_be)) {
                spdlog::warn("[Session {}] XEC block WITHHELD (eCash RTT) worker={} height={} "
                             "hash={} share_diff={:.4f} - found too soon after parent; avalanche "
                             "would park it (orphan). Share credited; not submitted.",
                             client_ip_, worker, job->height, block_hash_hex, share_diff);
                return;
            }
        }
    }

    std::string cb_for_block =
        job->segwit_active ? segwitify_coinbase(cb_hex) : cb_hex;
    std::string full_header_hex = std::string(header_hex);
    if (coin_.chain == ChainKind::Zcash) {
        full_header_hex.append("fd4005").append(solution_s);
    }
    std::string full_block = utils::assemble_full_block(
        full_header_hex, cb_for_block,
        std::span<const std::string>(job->tx_hexes.data(), job->tx_hexes.size()));
    if (!job->mweb.empty()) {
        full_block += "01" + job->mweb;
    }
    const int miner_id_for_block = miner_id_;
    const std::int64_t height_for_block = static_cast<std::int64_t>(job->height);
    const double diff_for_block = share_diff;
    // The callback deliberately captures no `this` (the session may be gone by
    // the time the RPC returns); btc_ is a shared_ptr copy so the precious
    // call stays valid. preciousblock shipped in Bitcoin Core 0.14 and exists
    // on every chain we run except zcashd (0.11-era fork), so skip Zcash.
    const bool use_precious = (coin_.chain != ChainKind::Zcash);
    btc_->asyncSubmitBlock(full_block,
        [worker, height = job->height, block_hash_hex, miner_id_for_block,
         height_for_block, diff_for_block, reward = job->coinbase_value,
         btc = btc_, use_precious]
        (const boost::system::error_code& ec, const nlohmann::json& rsp) {
            if (ec) {
                spdlog::error("[Session] submitblock RPC error worker={} h={}: {}",
                              worker, height, ec.message());
                return;
            }
            if (rsp.contains("error") && !rsp["error"].is_null()) {
                spdlog::error("[Session] submitblock rejected worker={} h={} error={}",
                              worker, height, rsp["error"].dump());
                return;
            }
            // Force our own node onto our block for same-height races. This
            // must run for soft results too: "duplicate"/"inconclusive" is
            // exactly the case where the node saw a competitor first and
            // needs the nudge.
            if (use_precious) mark_block_precious(btc, block_hash_hex);
            // submitblock returns null on clean accept, or a string reason
            // ("duplicate", "inconclusive", "high-hash", ...) with error=null.
            // Surface that reason for diagnostics, but DO NOT drop the block:
            // an "inconclusive" block may still become canonical, the
            // blocks.block_hash UNIQUE constraint dedupes a true "duplicate",
            // and confirmation tracking marks any loser as orphan later.
            if (rsp.contains("result") && rsp["result"].is_string()) {
                spdlog::warn("[Session] submitblock soft result worker={} h={} hash={}: {}",
                             worker, height, block_hash_hex,
                             rsp["result"].get<std::string>());
            }
            spdlog::info("[Session] BLOCK ACCEPTED worker={} h={} hash={}",
                         worker, height, block_hash_hex);
            metrics::inc_block_found();
            // Persist to the `blocks` table so the history survives
            // process restarts (Prometheus counters don't).
            db_worker().enqueue_block(BlockRecord{
                .height       = height_for_block,
                .block_hash   = block_hash_hex,
                .miner_id     = miner_id_for_block,
                .difficulty   = diff_for_block,
                .reward_value = reward});
        });
}

// ---------------------------------------------------------------------------
// Stratum V2 Implementation
// ---------------------------------------------------------------------------

void ClientSession::do_read_v2(std::size_t bytes_to_read) {
    if (closed_.load()) return;

    // Never hand asio a length read_buf_ cannot hold. boost::asio::buffer() takes
    // the size on trust, so an unclamped count here writes straight past the end
    // of the array. Callers pass "everything still outstanding for this frame",
    // which legitimately exceeds the buffer; reading a chunk at a time is safe
    // because process_v2_buffer() re-enters after each one and recomputes what is
    // still missing, so a large frame simply arrives over several reads.
    const std::size_t n = std::min(bytes_to_read, read_buf_.size());
    if (n == 0) return;

    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(read_buf_.data(), n),
        boost::asio::bind_executor(strand_, [self, n](const boost::system::error_code& ec, std::size_t bytes) {
            if (ec || bytes != n) {
                self->on_disconnect_local("read v2 error");
                return;
            }

            self->sv2_read_buffer_.insert(self->sv2_read_buffer_.end(),
                                          self->read_buf_.begin(),
                                          self->read_buf_.begin() + n);

            self->process_v2_buffer();
        })
    );
}

void ClientSession::process_v2_buffer() {
    if (closed_.load()) return;
    
    if (is_v2_encrypted_) {
        sv2::Header h;
        
        if (sv2_pending_header_.has_value()) {
            // We already decrypted the header, waiting for payload
            h = *sv2_pending_header_;
        } else {
            // Need to decrypt the header first
            if (sv2_read_buffer_.size() < 22) {
                do_read_v2(22 - sv2_read_buffer_.size());
                return;
            }
            
            uint8_t decrypted_header[6];
            if (!noise_state_.decrypt(sv2_read_buffer_.data(), 22, decrypted_header)) {
                on_disconnect_local("Failed to decrypt encrypted V2 message header");
                return;
            }
            
            sv2::Reader r(decrypted_header, 6);
            h.deserialize(r);
            
            spdlog::info("[SV2 Session {}] Decrypted header: ext={} type=0x{:02x} msg_len={}", client_ip_, h.extension_type, h.msg_type, h.msg_length);

            // Reject before the length is used to size a read or an allocation.
            if (!sv2::frame_length_ok(h.msg_length)) {
                spdlog::warn("[SV2 Session {}] oversized frame: msg_len={} exceeds max {}",
                             client_ip_, h.msg_length, sv2::MAX_MSG_LENGTH);
                on_disconnect_local("SV2 frame exceeds maximum length");
                return;
            }

            // Remove the 22-byte encrypted header from buffer, keep remaining data
            sv2_read_buffer_.erase(sv2_read_buffer_.begin(), sv2_read_buffer_.begin() + 22);
            sv2_pending_header_ = h;
        }
        
        // Now we need msg_length + 16 bytes of encrypted payload
        size_t encrypted_payload_required = h.msg_length + 16;
        if (sv2_read_buffer_.size() < encrypted_payload_required) {
            do_read_v2(encrypted_payload_required - sv2_read_buffer_.size());
            return;
        }
        
        std::vector<uint8_t> decrypted_payload(h.msg_length);
        if (h.msg_length > 0) {
            if (!noise_state_.decrypt(sv2_read_buffer_.data(), encrypted_payload_required, decrypted_payload.data())) {
                on_disconnect_local("Failed to decrypt encrypted V2 message payload");
                return;
            }
        }
        
        sv2_read_buffer_.erase(sv2_read_buffer_.begin(), sv2_read_buffer_.begin() + encrypted_payload_required);
        sv2_pending_header_.reset();
        
        try {
            process_message_v2(h, decrypted_payload);
        } catch (const std::exception& e) {
            spdlog::error("[SV2 Session {}] error processing encrypted message type 0x{:02x}: {}", client_ip_, h.msg_type, e.what());
        }
    } else {
        if (sv2_read_buffer_.size() < 6) {
            do_read_v2(6 - sv2_read_buffer_.size());
            return;
        }
        
        sv2::Reader r(sv2_read_buffer_);
        sv2::Header h;
        h.deserialize(r);

        // Same bound as the encrypted path: msg_length is peer-supplied and is
        // about to size a read and a payload copy.
        if (!sv2::frame_length_ok(h.msg_length)) {
            spdlog::warn("[SV2 Session {}] oversized frame: msg_len={} exceeds max {}",
                         client_ip_, h.msg_length, sv2::MAX_MSG_LENGTH);
            on_disconnect_local("SV2 frame exceeds maximum length");
            return;
        }

        size_t total_required = 6 + h.msg_length;
        if (sv2_read_buffer_.size() < total_required) {
            do_read_v2(total_required - sv2_read_buffer_.size());
            return;
        }
        
        std::vector<uint8_t> payload(sv2_read_buffer_.begin() + 6, sv2_read_buffer_.begin() + total_required);
        
        sv2_read_buffer_.erase(sv2_read_buffer_.begin(), sv2_read_buffer_.begin() + total_required);
        
        try {
            process_message_v2(h, payload);
        } catch (const std::exception& e) {
            spdlog::error("[SV2 Session {}] error processing message type 0x{:02x}: {}", client_ip_, h.msg_type, e.what());
        }
    }
    
    if (!sv2_read_buffer_.empty()) {
        process_v2_buffer();
    } else {
        do_read_v2(is_v2_encrypted_ ? 22 : 6);
    }
}

void ClientSession::process_message_v2(const sv2::Header& h, const std::vector<uint8_t>& payload) {
    if (h.msg_type == sv2::MSG_SETUP_CONNECTION) {
        sv2::Reader r(payload);
        sv2::SetupConnection msg;
        msg.deserialize(r);
        
        spdlog::info("[SV2 Session {}] SetupConnection protocol={} min_v={} max_v={} vendor={} device={}",
                     client_ip_, msg.protocol, msg.min_version, msg.max_version, msg.vendor, msg.device_id);
                     
        if (msg.protocol != 0 && msg.protocol != 2) {
            send_v2_error("unsupported-protocol");
            return;
        }
        
        sv2::SetupConnectionSuccess resp;
        resp.used_version = 2;
        // Success.flags is a different namespace from the request flags: bit 0
        // here is REQUIRES_FIXED_VERSION. We allow version rolling (and send
        // version_rolling_allowed=true in NewExtendedMiningJob), so this MUST be
        // 0. Previously we sent FLAG_REQUIRES_STANDARD_JOBS (== 1), which reads
        // as REQUIRES_FIXED_VERSION on the wire and contradicted the jobs/shares
        // we actually accept.
        resp.flags        = 0;
        
        sv2::Writer w;
        resp.serialize(w);
        send_v2_message(sv2::EXT_COMMON, sv2::MSG_SETUP_CONNECTION_SUCCESS, w);
        
        subscribed_ = true;
        authorized_ = true;
        publish_snapshot();  // control-plane snapshot (SV2 setup)
    }
    else if (h.msg_type == sv2::MSG_OPEN_STANDARD_MINING_CHANNEL) {
        sv2::Reader r(payload);
        sv2::OpenStandardMiningChannel msg;
        msg.deserialize(r);

        spdlog::info("[SV2 Session {}] OpenStandardMiningChannel request_id={} user='{}'",
                     client_ip_, msg.request_id, msg.user_identity);

        worker_address_ = msg.user_identity;
        size_t dot = worker_address_.find('.');
        if (dot != std::string::npos) {
            worker_name_ = worker_address_.substr(dot + 1);
            worker_address_ = worker_address_.substr(0, dot);
        } else {
            worker_name_ = "default";
        }
        
        auto decoded = address::decode(worker_address_);
        if (!decoded) {
            send_v2_open_channel_error(msg.request_id, "invalid-username-or-address");
            return;
        }

        // Open the channel IMMEDIATELY on the local (structural + network) address decode,
        // so probes/miners get the channel-open success without waiting for a node RPC
        // round-trip. The authoritative node validateaddress runs in the background
        // (validate_payout_async below); if it ever comes back invalid we drop the
        // connection before any share/block can be mined to a bad payout address.
        worker_script_hex_ = address::script_pubkey_hex(*decoded);
        sv2_channel_id_ = 1000 + (std::rand() % 10000);

        {
            double diff = coin_.activeTier.startingDifficulty;
            last_difficulty_ = diff;

            sv2::OpenStandardMiningChannelSuccess resp;
            resp.request_id = msg.request_id;
            resp.channel_id = sv2_channel_id_;
            resp.group_channel_id = 0;
            resp.target = diff_to_target_bytes(diff);
            resp.extranonce_prefix = {0x01, 0x02, 0x03, 0x04};

            sv2::Writer w;
            resp.serialize(w);
            send_v2_message(sv2::EXT_COMMON, sv2::MSG_OPEN_STANDARD_MINING_CHANNEL_SUCCESS, w);

            miner_id_ = Database::ensure_miner(worker_address_, worker_name_, client_ip_);
            db_worker().enqueue_presence({worker_address_, worker_name_, "online", client_ip_});

            if (auto j = strat_->latestJob()) {
                emit_job_v2(*j);
            }
        }

        validate_payout_async();
    }
    else if (h.msg_type == sv2::MSG_OPEN_EXTENDED_MINING_CHANNEL) {
        sv2::Reader r(payload);
        sv2::OpenExtendedMiningChannel msg;
        msg.deserialize(r);
        
        spdlog::info("[SV2 Session {}] OpenExtendedMiningChannel request_id={} user='{}' hashrate={} min_extranonce={}",
                     client_ip_, msg.request_id, msg.user_identity, msg.nominal_hash_rate, msg.min_extranonce_size);
        
        worker_address_ = msg.user_identity;
        size_t dot = worker_address_.find('.');
        if (dot != std::string::npos) {
            worker_name_ = worker_address_.substr(dot + 1);
            worker_address_ = worker_address_.substr(0, dot);
        } else {
            worker_name_ = "default";
        }
        
        // Solo payout: the coinbase pays the miner's OWN address (the username) directly.
        // Reject an unparseable username up front. We open the channel IMMEDIATELY on the
        // local (structural + network) address decode so the client does not wait for a
        // node RPC round-trip (marketplace probes hang up in ~1 RTT). The authoritative
        // node validateaddress runs in the background (validate_payout_async below) and
        // drops the connection if the address ever comes back invalid - before any
        // share/block can be mined to a bad payout address.
        auto decoded = address::decode(worker_address_);
        if (!decoded) {
            send_v2_open_channel_error(msg.request_id, "invalid-username-or-address");
            return;
        }

        worker_script_hex_ = address::script_pubkey_hex(*decoded);
        sv2_channel_id_ = 1000 + (std::rand() % 10000);
        is_extended_channel_ = true;

        {
            // Seed the initial target from the miner's DECLARED hashrate rather
            // than opening at the vardiff floor. A large SV2 order (e.g. Braiins
            // at 1 PH/s) starting at vardiffMin floods shares and forces vardiff
            // to ramp violently; the resulting big SetTarget jumps make the proxy
            // drop the connection. Opening near the right difficulty lets vardiff
            // merely fine-tune. 2^32 hashes per diff-1 share, so the difficulty
            // for the configured share rate is hashrate / (2^32 * shares_per_sec).
            double diff = coin_.activeTier.startingDifficulty;
            if (coin_.activeTier.vardiffEnabled && msg.nominal_hash_rate > 0.0f
                && coin_.targetSharesPerMinute > 0.0) {
                const double rate_hz = coin_.targetSharesPerMinute / 60.0;
                const double est = static_cast<double>(msg.nominal_hash_rate)
                                   / (4294967296.0 * rate_hz);
                diff = std::clamp(est, coin_.activeTier.vardiffMin,
                                       coin_.activeTier.vardiffMax);
                spdlog::info("[SV2 Session {}] seeding initial diff={:.0f} from nominal_hash_rate={:.3g}",
                             client_ip_, diff, static_cast<double>(msg.nominal_hash_rate));
            }
            last_difficulty_ = diff;
            vardiff_.set_current(diff);

            // Allocate extranonce: at least what they asked for, minimum 8 bytes.
            uint16_t extranonce_size = std::max<uint16_t>(msg.min_extranonce_size, 8);
            extranonce2_size_ = extranonce_size;

            sv2::OpenExtendedMiningChannelSuccess resp;
            resp.request_id = msg.request_id;
            resp.channel_id = sv2_channel_id_;
            resp.group_channel_id = 0;
            resp.target = diff_to_target_bytes(diff);
            resp.extranonce_size = extranonce_size;
            resp.extranonce_prefix = utils::hex_to_bytes(extranonce1_);

            sv2::Writer w;
            resp.serialize(w);
            send_v2_message(sv2::EXT_COMMON, sv2::MSG_OPEN_EXTENDED_MINING_CHANNEL_SUCCESS, w);

            subscribed_ = true;
            authorized_ = true;

            miner_id_ = Database::ensure_miner(worker_address_, worker_name_, client_ip_);
            db_worker().enqueue_presence({worker_address_, worker_name_, "online", client_ip_});
            publish_snapshot();  // control-plane snapshot (SV2 extended channel)

            if (auto j = strat_->latestJob()) {
                emit_job_v2(*j);
            }
        }

        validate_payout_async();
    }
    else if (h.msg_type == sv2::MSG_UPDATE_CHANNEL) {
        sv2::Reader r(payload);
        sv2::UpdateChannel msg;
        msg.deserialize(r);
        
        spdlog::info("[SV2 Session {}] UpdateChannel channel_id={} hashrate={}",
                     client_ip_, msg.channel_id, msg.nominal_hash_rate);
        // Acknowledge silently - no response needed per spec unless error
    }
    else if (h.msg_type == sv2::MSG_SUBMIT_SHARES_STANDARD) {
        sv2::Reader r(payload);
        sv2::SubmitSharesStandard msg;
        msg.deserialize(r);
        handle_submit_shares_v2(msg);
    }
    else if (h.msg_type == sv2::MSG_SUBMIT_SHARES_EXTENDED) {
        sv2::Reader r(payload);
        sv2::SubmitSharesExtended msg;
        msg.deserialize(r);
        handle_submit_shares_extended_v2(msg);
    }
    else if (h.msg_type == sv2::MSG_SET_CUSTOM_MINING_JOB) {
        sv2::Reader r(payload);
        sv2::SetCustomMiningJob msg;
        msg.deserialize(r);
        handle_set_custom_mining_job_v2(msg);
    }
    else if (h.msg_type == sv2::MSG_CLOSE_CHANNEL) {
        sv2::Reader r(payload);
        sv2::CloseChannel msg;
        msg.deserialize(r);
        // SV2 spec: CloseChannel is per-channel and requires NO response; the
        // receiver must simply stop sending messages for that channel. Aggregating
        // proxies (e.g. the SRI translator) send it routinely when a downstream V1
        // miner drops. We log it and keep the TCP connection alive - the client
        // owns the connection lifecycle and may still have other channels open.
        // (Per-channel job suppression belongs with the multi-channel refactor;
        // this session currently models a single active channel per connection.)
        spdlog::info("[SV2 Session {}] CloseChannel channel_id={} reason='{}'",
                     client_ip_, msg.channel_id, msg.reason_code);
    }
    else {
        spdlog::warn("[SV2 Session {}] Unknown message type 0x{:02x} (ext=0x{:04x}, len={})",
                     client_ip_, h.msg_type, h.extension_type, h.msg_length);
    }
}

void ClientSession::send_v2_message(uint16_t ext_type, uint8_t msg_type, const sv2::Writer& w) {
    if (closed_.load()) return;
    std::vector<uint8_t> frame = sv2::wrap_message(ext_type, msg_type, w);
    
    if (is_v2_encrypted_) {
        std::vector<uint8_t> encrypted_frame(22 + (frame.size() - 6) + 16);
        if (!noise_state_.encrypt(frame.data(), 6, encrypted_frame.data())) {
            on_disconnect_local("Failed to encrypt outgoing V2 message header");
            return;
        }
        if (frame.size() > 6) {
            if (!noise_state_.encrypt(frame.data() + 6, frame.size() - 6, encrypted_frame.data() + 22)) {
                on_disconnect_local("Failed to encrypt outgoing V2 message payload");
                return;
            }
        }
        std::string s(encrypted_frame.begin(), encrypted_frame.end());
        send_line(std::move(s));
    } else {
        std::string s(frame.begin(), frame.end());
        send_line(std::move(s));
    }
}

void ClientSession::send_v2_error(const std::string& err_code) {
    sv2::SetupConnectionError err;
    err.error_code = err_code;
    err.flags      = 0;
    
    sv2::Writer w;
    err.serialize(w);
    send_v2_message(sv2::EXT_COMMON, sv2::MSG_SETUP_CONNECTION_ERROR, w);
}

void ClientSession::send_v2_open_channel_error(uint32_t request_id, const std::string& err_code) {
    sv2::OpenMiningChannelError err;
    err.request_id = request_id;
    err.error_code = err_code;
    
    sv2::Writer w;
    err.serialize(w);
    send_v2_message(sv2::EXT_COMMON, sv2::MSG_OPEN_MINING_CHANNEL_ERROR, w);
}

void ClientSession::validate_payout_async() {
    // Authoritative node-side payout-address check, run OFF the channel-open critical
    // path. The channel is already open (we trust the local structural+network decode);
    // here we confirm with the node and drop the connection if it disagrees, before any
    // share/block is mined to a bad address. Solo-pool safety net for SV2 channels.
    auto self = shared_from_this();
    std::string vaddr = worker_address_;
    btc_->asyncValidateAddress(vaddr, [self, vaddr](const boost::system::error_code& ec, bool valid, bool, bool) {
        boost::asio::post(self->strand_, [self, vaddr, ec, valid]() {
            if (self->closed_.load()) return;
            if (ec || !valid) {
                spdlog::warn("[SV2 Session {}] payout address '{}' failed node validation, closing channel",
                             self->client_ip_, vaddr);
                self->on_disconnect_local("invalid payout address");
            }
        });
    });
}

void ClientSession::send_set_target(double d, std::optional<double> old_diff) {
    // Track previous difficulty for the transition grace window (same as set_difficulty).
    last_difficulty_ = old_diff.value_or(d);
    last_difficulty_change_ts_ = std::chrono::steady_clock::now();
    stat_diff_.store(std::round(d), std::memory_order_relaxed);  // control-plane display

    sv2::SetTarget st;
    st.channel_id = sv2_channel_id_;
    st.maximum_target = diff_to_target_bytes(d);

    sv2::Writer w;
    st.serialize(w);
    send_v2_message(sv2::EXT_COMMON, sv2::MSG_SET_TARGET, w);
    spdlog::info("[SV2 Session {}] -> SetTarget diff={:.0f}", client_ip_, std::round(d));
}

std::array<uint8_t, 32> ClientSession::diff_to_target_bytes(double diff) const {
    // Bitcoin difficulty-1 target (pdiff): 0xFFFF * 2^208.
    double max_target = 26959535291011309493156476344723991336010898738574164086137773096960.0;
    double target_val = max_target;
    if (diff > 0.0) {
        target_val /= diff;
    }
    
    std::array<uint8_t, 32> target_bytes{};
    double current = target_val;
    for (int i = 0; i < 32; ++i) {
        double rem = std::fmod(current, 256.0);
        target_bytes[i] = static_cast<uint8_t>(rem);
        current = std::floor(current / 256.0);
    }
    return target_bytes;
}

void ClientSession::emit_job_v2(const MiningJob& job) {
    uint32_t job_id_u32 = 0;
    if (job.job_id.size() >= 8) {
        try {
            job_id_u32 = static_cast<uint32_t>(std::stoul(job.job_id.substr(0, 8), nullptr, 16));
        } catch (...) {}
    }

    std::string cb2_session = build_session_coinbase2(job, worker_script_hex_);
    
    if (is_extended_channel_) {
        sv2::NewExtendedMiningJob emj;
        emj.channel_id = sv2_channel_id_;
        emj.job_id = job_id_u32;
        emj.min_ntime = std::nullopt;
        emj.version = job.version;
        emj.version_rolling_allowed = true;
        
        for (const auto& branch_hex : job.merkle_branch_le) {
            std::vector<uint8_t> branch_bytes = utils::hex_to_bytes(branch_hex);
            if (branch_bytes.size() == 32) {
                std::array<uint8_t, 32> hash_arr;
                std::memcpy(hash_arr.data(), branch_bytes.data(), 32);
                emj.merkle_path.push_back(hash_arr);
            }
        }
        
        emj.coinbase_tx_prefix = utils::hex_to_bytes(job.coinbase1);
        emj.coinbase_tx_suffix = utils::hex_to_bytes(cb2_session);
        
        sv2::Writer w;
        emj.serialize(w);
        send_v2_message(sv2::EXT_COMMON, sv2::MSG_NEW_EXTENDED_MINING_JOB, w);
    } else {
        // Standard channel: the miner receives only the finished merkle root.
        // Full-block mode (default, mainnet): normal coinbase (subsidy + fees, segwit
        // commitment) and the full merkle root over the template's branch.
        // Empty-block mode (testnet opt-in): subsidy-only legacy coinbase, coinbase-only root.
        std::string merkle_root_le;
        if (coin_.stratumV2EmptyBlocks) {
            MiningJob eb_job = job;
            eb_job.coinbase_value = job.coinbase_value - job.total_fees;
            eb_job.segwit_active = false;
            eb_job.witness_commitment.clear();
            std::string cb2_eb = build_session_coinbase2(eb_job, worker_script_hex_);
            std::string cb_hex = utils::rebuild_coinbase(job.coinbase1, cb2_eb, extranonce1_ + "0000000000000000");
            merkle_root_le = utils::compute_coinbase_txid_le(cb_hex);
        } else {
            std::string cb2 = build_session_coinbase2(job, worker_script_hex_);
            std::string cb_hex = utils::rebuild_coinbase(job.coinbase1, cb2, extranonce1_ + "0000000000000000");
            std::string cb_txid_le = utils::compute_coinbase_txid_le(cb_hex);
            merkle_root_le = merkle::compute_root_le_hex(cb_txid_le, job.merkle_branch_le);
        }

        sv2::NewMiningJob mj;
        mj.channel_id = sv2_channel_id_;
        mj.job_id     = job_id_u32;
        mj.version    = job.version;

        std::vector<uint8_t> mr_bytes = utils::hex_to_bytes(merkle_root_le);
        if (mr_bytes.size() == 32) {
            std::memcpy(mj.merkle_root.data(), mr_bytes.data(), 32);
        }

        sv2::Writer w;
        mj.serialize(w);
        send_v2_message(sv2::EXT_COMMON, sv2::MSG_NEW_MINING_JOB, w);
    }

    sv2::SetNewPrevHash ph;
    ph.channel_id = sv2_channel_id_;
    ph.job_id     = job_id_u32;
    ph.min_ntime  = job.mintime;
    ph.nbits      = 0;
    try {
        std::vector<uint8_t> bits_bytes = utils::hex_to_bytes(job.bits);
        if (bits_bytes.size() == 4) {
            ph.nbits = (bits_bytes[0] << 24) | (bits_bytes[1] << 16) | (bits_bytes[2] << 8) | bits_bytes[3];
        }
    } catch(...) {}
    
    // prev_hash goes into the block header in internal (reversed) byte order, the same
    // order the share validator and block assembly use. getblocktemplate gives the display
    // (big-endian) hash, so reverse it; otherwise the miner mines a header we never validate.
    std::vector<uint8_t> ph_bytes = utils::hex_to_bytes(utils::byte_reverse_hex(job.previousblockhash));
    if (ph_bytes.size() == 32) {
        std::memcpy(ph.prev_hash.data(), ph_bytes.data(), 32);
    }

    sv2::Writer w_ph;
    ph.serialize(w_ph);
    send_v2_message(sv2::EXT_COMMON, sv2::MSG_SET_NEW_PREV_HASH, w_ph);
}

void ClientSession::submit_block_sv2(const JobPtr& job,
                                     const std::string& header_hex,
                                     const std::string& cb_hex,
                                     const std::array<std::uint8_t, 32>& header_hash_be,
                                     double share_diff, bool empty_block) {
    // eCash RTT guard (see submit_block_candidate): withhold a too-soon block
    // avalanche would park/orphan. XEC-only; no-op for every other chain.
    if (job && coin_.chain == ChainKind::eCash && job->rtt_present) {
        if (!utils::ecash_rtt_ok(job->rtt_prevheadertime, job->rtt_prevbits,
                                 static_cast<std::int64_t>(now_unix()), header_hash_be)) {
            spdlog::warn("[SV2 Session {}] XEC block WITHHELD (eCash RTT) height={} hash={} "
                         "share_diff={:.4f} - found too soon after parent; avalanche would park "
                         "it (orphan). Not submitted.",
                         client_ip_, job->height,
                         utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()}),
                         share_diff);
            return;
        }
    }

    // Full-block (default): segwitify the coinbase on segwit chains and include
    // the template transactions. Empty-block (testnet opt-in): legacy coinbase,
    // no txs. Extended channels always mine full blocks (empty_block=false).
    std::string cb_for_block;
    std::vector<std::string> block_txs;
    if (empty_block) {
        cb_for_block = cb_hex;
    } else {
        cb_for_block = (job && job->segwit_active) ? segwitify_coinbase(cb_hex) : cb_hex;
        if (job) block_txs = job->tx_hexes;
    }
    std::string full_block = utils::assemble_full_block(
        header_hex, cb_for_block,
        std::span<const std::string>(block_txs.data(), block_txs.size()));
    if (job && !job->mweb.empty()) {
        full_block += "01" + job->mweb;
    }

    const std::string block_hash_hex =
        utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()});
    const int miner_id_for_block = miner_id_;
    const std::int64_t height_for_block = job ? static_cast<std::int64_t>(job->height) : 0;
    const double diff_for_block = share_diff;

    spdlog::info("[SV2 Session {}] BLOCK CANDIDATE share_diff={:.4f}", client_ip_, share_diff);
    btc_->asyncSubmitBlock(full_block,
        [block_hash_hex, miner_id_for_block, height_for_block, diff_for_block,
         btc = btc_, use_precious = (coin_.chain != ChainKind::Zcash)]
        (const boost::system::error_code& ec, const nlohmann::json& rsp) {
            if (ec) {
                spdlog::error("[SV2 Session] submitblock RPC error: {}", ec.message());
                return;
            }
            if (rsp.contains("error") && !rsp["error"].is_null()) {
                spdlog::error("[SV2 Session] submitblock rejected: {}", rsp["error"].dump());
                return;
            }
            // Same-height race nudge (see submit_block_candidate).
            if (use_precious) mark_block_precious(btc, block_hash_hex);
            spdlog::info("[SV2 Session] BLOCK ACCEPTED hash={}", block_hash_hex);
            metrics::inc_block_found();
            db_worker().enqueue_block(BlockRecord{
                .height       = height_for_block,
                .block_hash   = block_hash_hex,
                .miner_id     = miner_id_for_block,
                .difficulty   = diff_for_block,
                .reward_value = 0});
        });
}

void ClientSession::handle_submit_shares_v2(const sv2::SubmitSharesStandard& msg) {
    auto t0 = std::chrono::steady_clock::now();

    auto reject_v2 = [this, msg](const std::string& err_code) {
        spdlog::warn("[SV2 Session {}] share rejected ({}) worker='{}' channel_id={} job={:08x}",
                     client_ip_, err_code, worker_name_, sv2_channel_id_, msg.job_id);
        sv2::SubmitSharesError err;
        err.channel_id = sv2_channel_id_;
        err.sequence_number = msg.sequence_number;
        err.error_code = err_code;

        sv2::Writer w;
        err.serialize(w);
        send_v2_message(sv2::EXT_COMMON, sv2::MSG_SUBMIT_SHARES_ERROR, w);
        shares_rejected_.fetch_add(1, std::memory_order_relaxed);
        metrics::inc_share_rejected(err_code.c_str());
    };

    std::string header_hex;
    std::string cb_hex;
    std::string prev_hash_hex;
    std::string bits_hex;
    bool meets_network = false;
    double share_diff = 0.0;
    JobPtr job;

    auto cit = sv2_custom_jobs_.find(msg.job_id);
    if (cit != sv2_custom_jobs_.end()) {
        const auto& cj = cit->second;
        
        cb_hex += utils::le_hex_u32(cj.coinbase_tx_version);
        cb_hex += "01";
        cb_hex += std::string(64, '0');
        cb_hex += "ffffffff";
        
        size_t script_len = cj.coinbase_prefix.size();
        char lenbuf[3];
        std::snprintf(lenbuf, sizeof(lenbuf), "%02zx", script_len);
        cb_hex += lenbuf;
        cb_hex += utils::bytes_to_hex(cj.coinbase_prefix);
        
        cb_hex += utils::le_hex_u32(cj.coinbase_tx_input_nSequence);
        cb_hex += utils::bytes_to_hex(cj.coinbase_outputs);
        cb_hex += utils::le_hex_u32(cj.coinbase_tx_locktime);
        
        std::string cb_txid_le = utils::compute_coinbase_txid_le(cb_hex);
        std::string merkle_root_le = cb_txid_le;
        
        std::string version_hex_le = utils::le_hex_u32(msg.version);
        prev_hash_hex = utils::bytes_to_hex(cj.prev_hash);
        std::string ntime_hex = utils::le_hex_u32(msg.ntime);
        std::string nonce_hex = utils::le_hex_u32(msg.nonce);
        bits_hex = utils::int_to_hex(cj.nbits, 8);
        
        header_hex = utils::build_block_header(
            version_hex_le,
            prev_hash_hex,
            merkle_root_le,
            ntime_hex,
            bits_hex,
            nonce_hex
        );
    } else {
        if (auto j = strat_->latestJob()) {
            uint32_t jid = 0;
            if (j->job_id.size() >= 8) {
                try { jid = static_cast<uint32_t>(std::stoul(j->job_id.substr(0, 8), nullptr, 16)); } catch(...) {}
            }
            if (jid == msg.job_id) {
                job = j;
            }
        }
        if (!job) {
            // SV2 sends only a u32 job id; recover the exact recent job from the
            // window by that id (findJob's full-string match never matched it,
            // which is why non-latest-job shares fell back to the wrong job).
            job = strat_->findJobU32(msg.job_id);
        }
        if (!job) {
            // The referenced job has rotated out of the window: genuinely stale.
            // Do NOT reconstruct against a different (latest) job - that produced
            // a garbage hash that got rejected as difficulty-too-low.
            reject_v2("stale-share");
            return;
        }
        
        // Rebuild the exact same work emit_job_v2 sent (same 8-byte extranonce2 slot), so the
        // header we validate matches what the miner mined and the block we submit is valid.
        // Full-block (default): subsidy+fees coinbase, full merkle root. Empty-block (testnet
        // opt-in): subsidy-only legacy coinbase, coinbase-only root.
        std::string merkle_root_le;
        if (coin_.stratumV2EmptyBlocks) {
            MiningJob eb_job = *job;
            eb_job.coinbase_value = job->coinbase_value - job->total_fees;
            eb_job.segwit_active = false;
            eb_job.witness_commitment.clear();
            std::string cb2 = build_session_coinbase2(eb_job, worker_script_hex_);
            cb_hex = utils::rebuild_coinbase(job->coinbase1, cb2, extranonce1_ + "0000000000000000");
            merkle_root_le = utils::compute_coinbase_txid_le(cb_hex);
        } else {
            std::string cb2 = build_session_coinbase2(*job, worker_script_hex_);
            cb_hex = utils::rebuild_coinbase(job->coinbase1, cb2, extranonce1_ + "0000000000000000");
            std::string cb_txid_le = utils::compute_coinbase_txid_le(cb_hex);
            merkle_root_le = merkle::compute_root_le_hex(cb_txid_le, job->merkle_branch_le);
        }
        
        std::string version_hex_le = utils::le_hex_u32(msg.version);
        prev_hash_hex = utils::byte_reverse_hex(job->previousblockhash);
        std::string ntime_hex = utils::le_hex_u32(msg.ntime);
        std::string nonce_hex = utils::le_hex_u32(msg.nonce);
        bits_hex = utils::byte_reverse_hex(job->bits);
        
        header_hex = utils::build_block_header(
            version_hex_le,
            prev_hash_hex,
            merkle_root_le,
            ntime_hex,
            bits_hex,
            nonce_hex
        );
    }
    
    auto header_bytes = utils::hex_to_bytes(header_hex);
    std::array<std::uint8_t, 32> sha_out;
    if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin) {
        sha_out = utils::scrypt_1024_1_1_32({header_bytes.data(), header_bytes.size()});
    } else {
        sha_out = utils::sha256d_arr({header_bytes.data(), header_bytes.size()});
    }
    
    std::array<std::uint8_t, 32> header_hash_be{};
    for (std::size_t i = 0; i < 32; ++i) header_hash_be[i] = sha_out[31 - i];
    
    // Use the normal compact bits (1d00ffff) for the network target, not the
    // header-order reversed bits_hex (ffff001d) which target_from_bits_hex parses to 0.
    auto net_target_bytes = utils::target_from_bits_hex(job ? job->bits : bits_hex);
    auto le_byte_cmp = [](const auto& a, const auto& b) {
        for (std::size_t i = 0; i < 32; ++i) {
            if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        }
        return 0;
    };
    meets_network = le_byte_cmp(header_hash_be, net_target_bytes) <= 0;
    
    {
        long double hash_as_d = 0.0L, base = 1.0L;
        for (int i = 31; i >= 0; --i) {
            hash_as_d += (long double)header_hash_be[i] * base;
            base *= 256.0L;
        }
        long double max_target_d;
        if (coin_.chain == ChainKind::Zcash) {
            max_target_d = (long double)524287.0L * std::pow(2.0L, 224.0L);
        } else if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin) {
            max_target_d = (long double)0xFFFF * std::pow(2.0L, 224.0L);
        } else {
            max_target_d = (long double)0xFFFF * std::pow(2.0L, 208.0L);
        }
        if (hash_as_d > 0.0L) share_diff = (double)(max_target_d / hash_as_d);
    }

    // Never drop a block solve: submit it here, before the difficulty/stale
    // checks below, so a share below the assigned target still wins the block.
    // Gate on first-sighting so a replayed share cannot double-submit. Mirrors
    // the V1 stale/low-diff "submit anyway" guard.
    if (meets_network &&
        strat_->register_share(utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()}))) {
        submit_block_sv2(job, header_hex, cb_hex, header_hash_be, share_diff,
                         coin_.stratumV2EmptyBlocks);
    }

    // Difficulty check with a grace window for the vardiff transition: when we have
    // just sent a SetTarget, in-flight shares mined against the previous target are
    // still accepted for a short window (mirrors the V1 set_difficulty grace).
    double target_diff = vardiff_.current();
    const double tolerance = 0.99;
    if (share_diff < target_diff) {
        double grace_age = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - last_difficulty_change_ts_).count();
        if (grace_age < 40.0 && share_diff >= last_difficulty_ * tolerance) {
            target_diff = last_difficulty_;
        }
    }
    if (share_diff < target_diff * tolerance) {
        spdlog::info("[SV2 Session {}] low-diff share_diff={:.3f} vardiff={:.3f} (last_diff={:.3f})",
                     client_ip_, share_diff, vardiff_.current(), last_difficulty_);
        reject_v2("difficulty-too-low");
        return;
    }

    sv2::SubmitSharesSuccess resp;
    resp.channel_id = sv2_channel_id_;
    resp.last_sequence_number = msg.sequence_number;
    resp.new_submits_accepted_count = 1;
    resp.new_shares_sum = 1;

    sv2::Writer w_resp;
    resp.serialize(w_resp);
    send_v2_message(sv2::EXT_COMMON, sv2::MSG_SUBMIT_SHARES_SUCCESS, w_resp);

    shares_accepted_.fetch_add(1, std::memory_order_relaxed);
    note_accepted_share(share_diff);   // idle + best-share
    metrics::inc_share_accepted();

    // Vardiff retarget (v2): same controller as V1, but the new difficulty is pushed
    // via SetTarget instead of mining.set_difficulty. network_diff=0 skips the
    // cap-to-network clamp so a diff-1 testnet doesn't collapse the target.
    if (coin_.activeTier.vardiffEnabled) {
        double old_diff = vardiff_.current();
        if (auto nd = vardiff_.on_share(std::chrono::steady_clock::now(), target_diff, 0.0)) {
            send_set_target(*nd, old_diff);
        }
    }

    if (miner_id_ > 0) {
        db_worker().enqueue_share(ShareRecord{
            miner_id_, std::to_string(msg.job_id), utils::le_hex_u32(msg.nonce), extranonce1_, "0000000000000000",
            utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()}),
            target_diff, true, meets_network});
        db_worker().enqueue_best_share(BestShareRecord{
            miner_id_, share_diff,
            utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()})});
    }
    
    auto us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count();
    metrics::observe_share_processing_us(us);
}

void ClientSession::handle_submit_shares_extended_v2(const sv2::SubmitSharesExtended& msg) {
    auto t0 = std::chrono::steady_clock::now();
    if (closed_.load()) return;

    // Declared ahead of reject_v2 so the reject path can report the computed share
    // difficulty against the current channel target. share_diff < target_diff
    // ("difficulty-too-low") is the dominant SV2 reject cause - most often a
    // per-channel target desync when a proxy multiplexes many channels over one
    // connection. Both stay 0 for the early (pre-hash) reject paths.
    double share_diff = 0.0;
    double target_diff = 0.0;

    auto reject_v2 = [&](const std::string& err_code) {
        spdlog::warn("[SV2 Session {}] share rejected ({}) worker='{}' channel_id={} "
                     "job={:08x} share_diff={:.4f} target_diff={:.0f}",
                     client_ip_, err_code, worker_name_, sv2_channel_id_,
                     msg.job_id, share_diff, target_diff);
        sv2::SubmitSharesError err;
        err.channel_id = sv2_channel_id_;
        err.sequence_number = msg.sequence_number;
        err.error_code = err_code;

        sv2::Writer w;
        err.serialize(w);
        send_v2_message(sv2::EXT_COMMON, sv2::MSG_SUBMIT_SHARES_ERROR, w);
        shares_rejected_.fetch_add(1, std::memory_order_relaxed);
        metrics::inc_share_rejected(err_code.c_str());
    };

    std::string header_hex;
    std::string cb_hex;
    std::string prev_hash_hex;
    std::string bits_hex;
    bool meets_network = false;
    JobPtr job;

    if (auto j = strat_->latestJob()) {
        uint32_t jid = 0;
        if (j->job_id.size() >= 8) {
            try { jid = static_cast<uint32_t>(std::stoul(j->job_id.substr(0, 8), nullptr, 16)); } catch(...) {}
        }
        if (jid == msg.job_id) {
            job = j;
        }
    }
    if (!job) {
        // SV2 sends only a u32 job id; recover the exact recent job from the
        // window by that id (findJob's full-string match never matched it, which
        // is why non-latest-job shares fell back to the wrong latest job and
        // reconstructed to a garbage hash rejected as difficulty-too-low).
        job = strat_->findJobU32(msg.job_id);
    }
    if (!job) {
        // The referenced job has rotated out of the window: genuinely stale.
        reject_v2("stale-share");
        return;
    }
    
    std::string cb2_session = build_session_coinbase2(*job, worker_script_hex_);
    std::string extranonce_hex = utils::bytes_to_hex(msg.extranonce);
    cb_hex = utils::rebuild_coinbase(job->coinbase1, cb2_session, extranonce1_ + extranonce_hex);
    std::string cb_txid_le = utils::compute_coinbase_txid_le(cb_hex);
    std::string merkle_root_le = merkle::compute_root_le_hex(cb_txid_le, job->merkle_branch_le);
    
    std::string version_hex_le = utils::le_hex_u32(msg.version);
    prev_hash_hex = utils::byte_reverse_hex(job->previousblockhash);
    std::string ntime_hex = utils::le_hex_u32(msg.ntime);
    std::string nonce_hex = utils::le_hex_u32(msg.nonce);
    bits_hex = utils::byte_reverse_hex(job->bits);
    
    header_hex = utils::build_block_header(
        version_hex_le,
        prev_hash_hex,
        merkle_root_le,
        ntime_hex,
        bits_hex,
        nonce_hex
    );
    
    auto header_bytes = utils::hex_to_bytes(header_hex);
    std::array<std::uint8_t, 32> sha_out;
    if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin) {
        sha_out = utils::scrypt_1024_1_1_32({header_bytes.data(), header_bytes.size()});
    } else {
        sha_out = utils::sha256d_arr({header_bytes.data(), header_bytes.size()});
    }
    
    std::array<std::uint8_t, 32> header_hash_be{};
    for (std::size_t i = 0; i < 32; ++i) header_hash_be[i] = sha_out[31 - i];
    
    // Use the normal compact bits (1d00ffff) for the network target, not the
    // header-order reversed bits_hex (ffff001d) which target_from_bits_hex parses to 0.
    auto net_target_bytes = utils::target_from_bits_hex(job ? job->bits : bits_hex);
    auto le_byte_cmp = [](const auto& a, const auto& b) {
        for (std::size_t i = 0; i < 32; ++i) {
            if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        }
        return 0;
    };
    meets_network = le_byte_cmp(header_hash_be, net_target_bytes) <= 0;
    
    {
        long double hash_as_d = 0.0L, base = 1.0L;
        for (int i = 31; i >= 0; --i) {
            hash_as_d += (long double)header_hash_be[i] * base;
            base *= 256.0L;
        }
        long double max_target_d;
        if (coin_.chain == ChainKind::Zcash) {
            max_target_d = (long double)524287.0L * std::pow(2.0L, 224.0L);
        } else if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin) {
            max_target_d = (long double)0xFFFF * std::pow(2.0L, 224.0L);
        } else {
            max_target_d = (long double)0xFFFF * std::pow(2.0L, 208.0L);
        }
        if (hash_as_d > 0.0L) share_diff = (double)(max_target_d / hash_as_d);
    }

    // Never drop a block solve: submit it here, before the difficulty/stale
    // checks below, so a share below the assigned target still wins the block.
    // Gate on first-sighting so a replayed share cannot double-submit. Mirrors
    // the V1 stale/low-diff "submit anyway" guard. Extended channels always mine
    // full blocks (empty_block=false).
    if (meets_network &&
        strat_->register_share(utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()}))) {
        submit_block_sv2(job, header_hex, cb_hex, header_hash_be, share_diff,
                         /*empty_block=*/false);
    }

    target_diff = vardiff_.current();
    bool grace_applied = false;
    double grace_age = 0.0;
    
    if (share_diff < target_diff) {
        grace_age = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_difficulty_change_ts_).count();
        if (grace_age < 15.0 && share_diff >= last_difficulty_) {
            target_diff = last_difficulty_;
            grace_applied = true;
        }
    }
    
    if (share_diff < target_diff) {
        reject_v2("difficulty-too-low");
        return;
    }

    // Same line the V1 path emits (handle_submit), so the two protocols stay
    // greppable together. Only fires inside the grace window after a retarget,
    // so it is rare: this is the share that would have been rejected had it
    // arrived a moment later -- exactly what you want when a miner reports
    // unexplained rejects.
    if (grace_applied) {
        spdlog::info("[SV2 Session {}] grace-accepted share_diff={:.3f} under old difficulty {:.3f} (new target was {:.3f}, age={:.1f}s)",
                     client_ip_, share_diff, last_difficulty_, vardiff_.current(), grace_age);
    }
    
    if (!utils::valid_ntime(msg.ntime, job->mintime, now_unix())) {
        reject_v2("time-too-old-or-new");
        return;
    }
    
    sv2::SubmitSharesSuccess resp;
    resp.channel_id = sv2_channel_id_;
    resp.last_sequence_number = msg.sequence_number;
    resp.new_submits_accepted_count = 1;
    resp.new_shares_sum = 1;
    
    sv2::Writer w_resp;
    resp.serialize(w_resp);
    send_v2_message(sv2::EXT_COMMON, sv2::MSG_SUBMIT_SHARES_SUCCESS, w_resp);

    shares_accepted_.fetch_add(1, std::memory_order_relaxed);
    note_accepted_share(share_diff);   // idle + best-share
    metrics::inc_share_accepted();

    // Vardiff tick - same as the V1 path (handle_submit). Without this the
    // extended channel stays pinned at the initial (floor) difficulty for the
    // whole session, flooding large SV2 orders with shares at diff = vardiffMin.
    // SV2 retargets via SetTarget rather than mining.set_difficulty.
    if (coin_.activeTier.vardiffEnabled && !has_custom_difficulty_) {
        double network_diff = 0.0;
        {
            long double net_d = 0.0L, base = 1.0L;
            for (int i = 31; i >= 0; --i) {
                net_d += (long double)net_target_bytes[i] * base;
                base *= 256.0L;
            }
            long double max_t;
            if (coin_.chain == ChainKind::Zcash) {
                max_t = (long double)524287.0L * std::pow(2.0L, 224.0L);
            } else if (coin_.chain == ChainKind::Litecoin || coin_.chain == ChainKind::Dogecoin) {
                max_t = (long double)0xFFFF * std::pow(2.0L, 224.0L);
            } else {
                max_t = (long double)0xFFFF * std::pow(2.0L, 208.0L);
            }
            if (net_d > 0.0L) network_diff = (double)(max_t / net_d);
        }
        double old_diff = vardiff_.current();
        if (auto nd = vardiff_.on_share(std::chrono::steady_clock::now(),
                                        target_diff, network_diff)) {
            send_set_target(*nd, old_diff);
        }
    }

    if (miner_id_ > 0) {
        db_worker().enqueue_share(ShareRecord{
            miner_id_, std::to_string(msg.job_id), utils::le_hex_u32(msg.nonce), extranonce1_, extranonce_hex,
            utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()}),
            target_diff, true, meets_network});
        db_worker().enqueue_best_share(BestShareRecord{
            miner_id_, share_diff,
            utils::bytes_to_hex({header_hash_be.data(), header_hash_be.size()})});
    }
    
    auto us = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count();
    metrics::observe_share_processing_us(us);
}

void ClientSession::handle_set_custom_mining_job_v2(const sv2::SetCustomMiningJob& msg) {
    if (!msg.merkle_path.empty()) {
        sv2::SetCustomMiningJobError err;
        err.channel_id = sv2_channel_id_;
        err.request_id = msg.request_id;
        err.error_code = "invalid-merkle-path-not-empty";
        
        sv2::Writer w;
        err.serialize(w);
        send_v2_message(sv2::EXT_COMMON, sv2::MSG_SET_CUSTOM_MINING_JOB_ERROR, w);
        return;
    }
    
    std::string donation_script;
    if (coin_.donationPercent > 0.0 && !coin_.donationAddress.empty()) {
        auto don_decoded = address::decode(coin_.donationAddress);
        if (don_decoded) {
            donation_script = address::script_pubkey_hex(*don_decoded);
        }
    }
    
    if (!donation_script.empty()) {
        std::string outputs_hex = utils::bytes_to_hex(msg.coinbase_tx_outputs);
        if (outputs_hex.find(donation_script) == std::string::npos) {
            sv2::SetCustomMiningJobError err;
            err.channel_id = sv2_channel_id_;
            err.request_id = msg.request_id;
            err.error_code = "invalid-coinbase-outputs-no-donation";
            
            sv2::Writer w;
            err.serialize(w);
            send_v2_message(sv2::EXT_COMMON, sv2::MSG_SET_CUSTOM_MINING_JOB_ERROR, w);
            return;
        }
    }
    
    uint32_t job_id = ++sv2_custom_job_id_counter_;
    CustomJob cj;
    cj.job_id                      = job_id;
    cj.version                     = msg.version;
    cj.prev_hash                   = msg.prev_hash;
    cj.coinbase_prefix             = msg.coinbase_prefix;
    cj.coinbase_outputs            = msg.coinbase_tx_outputs;
    cj.coinbase_tx_version         = msg.coinbase_tx_version;
    cj.coinbase_tx_input_nSequence = msg.coinbase_tx_input_nSequence;
    cj.coinbase_tx_locktime        = msg.coinbase_tx_locktime;
    cj.nbits                       = msg.nbits;
    
    sv2_custom_jobs_[job_id] = std::move(cj);
    
    sv2::SetCustomMiningJobSuccess resp;
    resp.channel_id = sv2_channel_id_;
    resp.request_id = msg.request_id;
    resp.job_id     = job_id;
    
    sv2::Writer w;
    resp.serialize(w);
    send_v2_message(sv2::EXT_COMMON, sv2::MSG_SET_CUSTOM_MINING_JOB_SUCCESS, w);
}

} // namespace mkpool
