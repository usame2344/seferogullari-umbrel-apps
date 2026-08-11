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
// File:        bitcoin_client.cpp
// Description: Bitcoin-family JSON-RPC client: getblocktemplate, submitblock, node failover.
// Created:     2025-03-30
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "bitcoin_client.hpp"
#include "logger.hpp"
#include <boost/beast/core/detail/base64.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>

namespace mkpool
{
	namespace bitcoin
	{
		using boost::asio::ip::tcp;

		namespace {
			// Block submission is the one RPC where a transient transport failure
			// can cost a found block, so we retry a few times with a short backoff
			// (ckpool retries submitblock up to 5x). This bounds ONLY the
			// no-response case; a real node answer is never retried.
			constexpr int  kSubmitMaxAttempts = 5;
			constexpr auto kSubmitRetryDelay  = std::chrono::milliseconds(250);
		}

		// Retry state for a single submitblock/submitauxblock call. Nested in
		// BitcoinClient so it can touch io_context_; holds a shared_ptr back to
		// the client so the object outlives every scheduled retry.
		struct BitcoinClient::SubmitRetryState {
			std::shared_ptr<BitcoinClient> self;
			json                           request;
			RPCCallback                    callback;
			int                            attempt{0};
			boost::asio::steady_timer      timer;
			SubmitRetryState(std::shared_ptr<BitcoinClient> s, json req, RPCCallback cb)
				: self(std::move(s)), request(std::move(req)), callback(std::move(cb)),
				  timer(self->io_context_) {}
		};

		void BitcoinClient::submitAttempt(const std::shared_ptr<SubmitRetryState>& st)
		{
			++st->attempt;
			const int attempt_no = st->attempt;
			asyncRPC(st->request,
				[this, st, attempt_no](const boost::system::error_code& ec, const json& resp) {
					// A node answer (clean accept OR rejection/soft result) comes
					// back with ec unset - deliver it verbatim, identical to the
					// pre-retry path. Only a set ec means "no valid response".
					if (!ec) {
						st->callback(ec, resp);
						return;
					}
					if (attempt_no >= kSubmitMaxAttempts) {
						mkpool::Logger::error(
							"submitblock: no node response after {} attempt(s): {} - giving up",
							attempt_no, ec.message());
						st->callback(ec, resp);
						return;
					}
					mkpool::Logger::error(
						"submitblock: transport failure attempt {}/{} ({}); retrying in {}ms",
						attempt_no, kSubmitMaxAttempts, ec.message(),
						static_cast<long long>(kSubmitRetryDelay.count()));
					st->timer.expires_after(kSubmitRetryDelay);
					st->timer.async_wait([this, st](const boost::system::error_code& tec) {
						if (tec) return;  // timer cancelled (shutdown)
						submitAttempt(st);
					});
				});
		}

		void BitcoinClient::asyncSubmitWithRetry(json request, RPCCallback callback)
		{
			auto st = std::make_shared<SubmitRetryState>(
				shared_from_this(), std::move(request), std::move(callback));
			submitAttempt(st);
		}

		BitcoinClient::BitcoinClient(boost::asio::io_context& io_context,
			const std::string& host,
			const std::string& port,
			const std::string& rpcUser,
			const std::string& rpcPassword,
			bool bchMode,
			ChainKind chainKind)
			: io_context_(io_context),
			socket_(io_context),
			host_(host),
			port_(port),
			bchMode_(bchMode),
			chainKind_(chainKind)
		{
			if (!rpcUser.empty() && !rpcPassword.empty())
            {
				authHeader_ = encodeAuth(rpcUser, rpcPassword);
			}
			// The primary node is always endpoint 0. Fallbacks append after
			// it via addFallbackNode(); with just this one entry the RPC path
			// stays single-endpoint and byte-identical to the original code.
			endpoints_.push_back(RpcEndpoint{host_, port_, authHeader_});
		}

		std::string BitcoinClient::encodeAuth(const std::string& user, const std::string& pass)
		{
			if (user.empty() && pass.empty()) return {};
			std::string auth = user + ":" + pass;
			std::size_t out_size = boost::beast::detail::base64::encoded_size(auth.size());
			std::vector<char> buffer(out_size);
			std::size_t n = boost::beast::detail::base64::encode(buffer.data(), auth.data(), auth.size());
			return std::string(buffer.data(), n);
		}

		void BitcoinClient::addFallbackNode(const std::string& host, const std::string& port,
			const std::string& rpcUser, const std::string& rpcPassword)
		{
			endpoints_.push_back(RpcEndpoint{host, port, encodeAuth(rpcUser, rpcPassword)});
			mkpool::Logger::info("[BitcoinClient] fallback node registered: {}:{} (total {} nodes)",
				host, port, endpoints_.size());
		}

		void BitcoinClient::addSubmitEndpoint(const std::string& host, const std::string& port,
			const std::string& rpcUser, const std::string& rpcPassword)
		{
			submitEndpoints_.push_back(RpcEndpoint{host, port, encodeAuth(rpcUser, rpcPassword)});
			mkpool::Logger::info("[BitcoinClient] redundant submit endpoint registered: {}:{}", host, port);
		}

		// asyncRPC: single-endpoint fast path (unchanged behaviour) or, when
		// fallbacks are configured, transport-failure failover across nodes.
		void BitcoinClient::asyncRPC(const json& request, RPCCallback callback)
		{
			if (endpoints_.size() <= 1) {
				auto client = std::make_shared<mkpool::BeastClient>(io_context_, host_, port_, authHeader_);
				client->run(request, std::move(callback));
				return;
			}
			rpcTryEndpoint(std::make_shared<json>(request),
				std::make_shared<RPCCallback>(std::move(callback)),
				current_.load(), 0);
		}

		void BitcoinClient::rpcTryEndpoint(std::shared_ptr<json> request,
			std::shared_ptr<RPCCallback> callback, std::size_t idx, std::size_t tried)
		{
			const auto& ep = endpoints_[idx];
			auto self = shared_from_this();
			auto client = std::make_shared<mkpool::BeastClient>(io_context_, ep.host, ep.port, ep.authHeader);
			client->run(*request,
				[this, self, request, callback, idx, tried](const boost::system::error_code& ec, const json& resp) {
					// A node ANSWER (accept or RPC-level error) arrives with ec
					// unset - deliver it verbatim and stick to this node. Only a
					// set ec (no valid response) triggers failover.
					if (!ec) {
						current_.store(idx);
						(*callback)(ec, resp);
						return;
					}
					const std::size_t next_tried = tried + 1;
					if (next_tried >= endpoints_.size()) {
						mkpool::Logger::error("[BitcoinClient] all {} nodes failed to respond: {}",
							endpoints_.size(), ec.message());
						(*callback)(ec, resp);
						return;
					}
					const std::size_t next_idx = (idx + 1) % endpoints_.size();
					mkpool::Logger::warn("[BitcoinClient] node {}:{} unreachable ({}); failing over to {}:{}",
						endpoints_[idx].host, endpoints_[idx].port, ec.message(),
						endpoints_[next_idx].host, endpoints_[next_idx].port);
					rpcTryEndpoint(request, callback, next_idx, next_tried);
				});
		}

		void BitcoinClient::startWatchdog()
		{
			if (endpoints_.size() <= 1) return;  // nothing to fail back to
			watchdog_ = std::make_unique<boost::asio::steady_timer>(io_context_);
			scheduleWatchdog();
			mkpool::Logger::info("[BitcoinClient] node watchdog started ({} nodes)", endpoints_.size());
		}

		void BitcoinClient::scheduleWatchdog()
		{
			watchdog_->expires_after(std::chrono::seconds(30));
			auto self = shared_from_this();
			watchdog_->async_wait([this, self](const boost::system::error_code& ec) {
				if (ec) return;  // cancelled on shutdown
				if (current_.load() != 0) {
					// We are on a fallback; probe the primary and switch back if
					// it has recovered. getblockcount is cheap and universal.
					auto client = std::make_shared<mkpool::BeastClient>(
						io_context_, endpoints_[0].host, endpoints_[0].port, endpoints_[0].authHeader);
					json probe;
					probe["method"] = BitcoinCommands::GetBlockCount;
					probe["params"] = json::array();
					probe["id"] = 1;
					auto self2 = shared_from_this();
					client->run(probe, [this, self2](const boost::system::error_code& pec, const json&) {
						if (!pec) {
							mkpool::Logger::info("[BitcoinClient] primary node recovered; switching back");
							current_.store(0);
						}
					});
				}
				scheduleWatchdog();
			});
		}

		void BitcoinClient::broadcastSubmit(const json& request)
		{
			for (const auto& ep : submitEndpoints_) {
				auto client = std::make_shared<mkpool::BeastClient>(io_context_, ep.host, ep.port, ep.authHeader);
				const std::string h = ep.host, p = ep.port;
				client->run(request, [h, p](const boost::system::error_code& ec, const json&) {
					if (ec) mkpool::Logger::warn("[BitcoinClient] redundant submitblock to {}:{} failed: {}",
						h, p, ec.message());
					else    mkpool::Logger::info("[BitcoinClient] redundant submitblock to {}:{} delivered", h, p);
				});
			}
		}

        //
        // ============================================================================
        // ADDRESS VALIDATION & METADATA
        // ============================================================================
        //

        /// \brief Validate a Bitcoin address using "validateaddress".
        void BitcoinClient::asyncValidateAddress(
            const std::string& address,
            std::function<void(const boost::system::error_code&, bool valid, bool script, bool segwit)> callback)
        {
            json request;
            request["method"] = BitcoinCommands::ValidateAddress;
            request["params"] = { address };
            request["id"] = 1;

            asyncRPC(request, [callback, address](const boost::system::error_code& ec, const json& response) {
                if (ec) {
                    callback(ec, false, false, false);
                    return;
                }
                try {
                    const auto& result = response.at("result");
                    bool is_valid = result.value("isvalid", false);
                    bool isscript = result.value("isscript", false);
                    bool iswitness = result.value("iswitness", false);

                    // Fallback detection
                    if (!result.contains("isscript") && !address.empty() &&
                        (address[0] == '3' || address[0] == '2'))
                    {
                        isscript = true;
                    }

                    callback({}, is_valid, isscript, iswitness);
                }
                catch (...) {
                    callback(boost::asio::error::invalid_argument, false, false, false);
                }
                });
        }

        /// \brief Detailed extended metadata for an address via "getaddressinfo".
        void BitcoinClient::asyncGetAddressInfo(const std::string& address, RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetAddressInfo;
            req["params"] = { address };
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief List unspent outputs via "listunspent".
        void BitcoinClient::asyncListUnspent(
            int minConf,
            int maxConf,
            const std::vector<std::string>& addresses,
            RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::ListUnspent;
            req["params"] = { minConf, maxConf, addresses };
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        //
        // ============================================================================
        // TRANSACTIONS
        // ============================================================================
        //

        /// \brief Decode a raw transaction using "decoderawtransaction".
        void BitcoinClient::asyncValidateTxn(const std::string& txn, RPCCallback callback)
        {
            if (txn.empty()) {
                callback(boost::asio::error::invalid_argument, json());
                return;
            }
            json req;
            req["method"] = BitcoinCommands::DecodeRawTransaction;
            req["params"] = { txn };
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Submit a raw transaction using "submittransaction".
        void BitcoinClient::asyncSubmitTxn(const std::string& txnData, RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::SubmitTransaction;
            req["params"] = { txnData };
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Get a raw transaction (verbose=1) using "getrawtransaction".
        void BitcoinClient::asyncGetTxn(const std::string& txHash, RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetRawTransaction;
            req["params"] = { txHash, 1 };
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        //
        // ============================================================================
        // BLOCKCHAIN / BLOCKS
        // ============================================================================
        //

        /// \brief Retrieve blockchain information via "getblockchaininfo".
        void BitcoinClient::asyncGetBlockchainInfo(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetBlockchainInfo;
            req["params"] = json::array();
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Retrieve the block count via "getblockcount".
        void BitcoinClient::asyncGetBlockCount(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetBlockCount;
            req["params"] = json::array();
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Get block hash by height using "getblockhash".
        void BitcoinClient::asyncGetBlockHash(int height, RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetBlockHash;
            req["params"] = { height };
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Get the best block hash via "getbestblockhash".
        void BitcoinClient::asyncGetBestBlockHash(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetBestBlockHash;
            req["params"] = json::array();
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Retrieve a block by hash using "getblock".
        void BitcoinClient::asyncGetBlock(const std::string& blockHash, RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetBlock;
            req["params"] = { blockHash };
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Submit a mined block using "submitblock" (with transport retry).
        void BitcoinClient::asyncSubmitBlock(const std::string& blockData, RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::SubmitBlock;
            req["params"] = { blockData };
            req["id"] = 1;
            // Redundant propagation: also push the block to any extra nodes,
            // fire-and-forget. Done BEFORE the move so the primary submit (with
            // retry) is unaffected. No-op when no extra endpoints are configured.
            if (!submitEndpoints_.empty()) broadcastSubmit(req);
            asyncSubmitWithRetry(std::move(req), std::move(callback));
        }

        void BitcoinClient::asyncGetAuxBlock(const std::string& payoutAddress, RPCCallback callback)
        {
            json req;
            req["method"] = "createauxblock";
            req["params"] = { payoutAddress };
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        void BitcoinClient::asyncSubmitAuxBlock(const std::string& hash, const std::string& auxpowHex, RPCCallback callback)
        {
            json req;
            req["method"] = "submitauxblock";
            req["params"] = { hash, auxpowHex };
            req["id"] = 1;
            asyncSubmitWithRetry(std::move(req), std::move(callback));
        }

        /// \brief Mark a block as precious via "preciousblock".
        void BitcoinClient::asyncPreciousBlock(const std::string& blockHash, RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::PreciousBlock;
            req["params"] = { blockHash };
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        //
        // ============================================================================
        // BLOCK TEMPLATE / MINING
        // ============================================================================
        //

        /// \brief Request new block template using "getblocktemplate".
        void BitcoinClient::asyncGetBlockTemplate(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetBlockTemplate;
            // BCH nodes reject/ignore the "segwit" rule (no segwit on BCH).
            // bchpool sends an empty-string rule entry; we follow the same
            // shape to stay maximally compatible with BCHN / BU.
            json rules;
            if (chainKind_ == ChainKind::BitcoinCash || chainKind_ == ChainKind::eCash) {
                rules = json::array({""});
            } else if (chainKind_ == ChainKind::Dogecoin) {
                // Dogecoin has no segwit; sending the "segwit" rule is rejected.
                rules = json::array({""});
            } else if (chainKind_ == ChainKind::Litecoin) {
                rules = json::array({"segwit", "mweb"});
            } else {
                rules = json::array({"segwit"});
            }
            if (chainKind_ == ChainKind::DigiByte) {
                req["params"] = {
                    {
                        { "capabilities", {"coinbasetxn", "workid", "coinbase/append"} },
                        { "rules", rules }
                    },
                    "sha256d"
                };
            } else {
                req["params"] = {
                    {
                        { "capabilities", {"coinbasetxn", "workid", "coinbase/append"} },
                        { "rules", rules }
                    }
                };
            }
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Retrieve mining information via "getmininginfo".
        void BitcoinClient::asyncGetMiningInfo(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetMiningInfo;
            req["params"] = json::array();
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        //
        // ============================================================================
        // WALLET
        // ============================================================================
        //

        /// \brief Retrieve total wallet balance using "getbalance".
        void BitcoinClient::asyncGetBalance(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetBalance;
            req["params"] = json::array();
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        //
        // ============================================================================
        // NETWORK / PEER INFO
        // ============================================================================
        //

        /// \brief Retrieve P2P & network information via "getnetworkinfo".
        void BitcoinClient::asyncGetNetworkInfo(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetNetworkInfo;
            req["params"] = json::array();
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Retrieve connected peer data via "getpeerinfo".
        void BitcoinClient::asyncGetPeerInfo(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetPeerInfo;
            req["params"] = json::array();
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        /// \brief Retrieve the number of connections using "getconnectioncount".
        void BitcoinClient::asyncGetConnectionCount(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetConnectionCount;
            req["params"] = json::array();
            req["id"] = 1;
            asyncRPC(req, callback);
        }

        //
        // ============================================================================
        // LEGACY
        // ============================================================================
        //

        /// \brief Legacy information command "getinfo".
        void BitcoinClient::asyncGetInfo(RPCCallback callback)
        {
            json req;
            req["method"] = BitcoinCommands::GetInfo;
            req["params"] = json::array();
            req["id"] = 1;
            asyncRPC(req, callback);
        }
	}
}