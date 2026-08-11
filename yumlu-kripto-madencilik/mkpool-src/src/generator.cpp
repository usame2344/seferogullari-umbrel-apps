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
// File:        generator.cpp
// Description: Block-template generator: GBT polling / ZMQ-driven refresh and job construction.
// Created:     2025-04-08
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#include "pch.hpp"
#include "generator.hpp"
#include "logger.hpp"
#include "utils.hpp"

namespace mkpool {

	Generator::Generator(boost::asio::io_context& io_context,
		std::shared_ptr<bitcoin::BitcoinClient> bitcoinClient,
		bool useZMQ,
		std::chrono::seconds pollInterval)
		: io_context_(io_context),
		bitcoinClient_(std::move(bitcoinClient)),
		pollTimer_(io_context),
		pollInterval_(pollInterval),
		running_(false),
		useZMQ_(useZMQ),
		zmqConnected_(false)
	{
		if (!useZMQ_) {
			mkpool::Logger::info("[Generator] Using RPC polling with interval: {} seconds", pollInterval_.count());
		}
	}

	Generator::~Generator()
	{
		stop();
	}

	void Generator::start()
	{
		{
			std::lock_guard<std::mutex> lock(mtx_);
			running_ = true;
			// Fallback timeout: only use RPC polling if ZMQ is truly silent.
			// On testnet (low tx volume) and after we dropped 'rawtx' the only
			// expected events are 'hashblock' (~10 min apart on mainnet, sparse
			// on testnet). Use 30 minutes so we don't spuriously fall back.
			fallbackTimeout_ = std::chrono::minutes(30);
			lastZmqEvent_ = std::chrono::steady_clock::now();
		}
		mkpool::Logger::info("[Generator] Starting...");

		if (useZMQ_) {
			startZMQ();
		}
		else {
			startRPCPolling();
		}

		// Begin periodic (30s) job rebroadcast keepalive.
		armJobUpdate();
	}

	void Generator::stop()
	{
		std::lock_guard<std::mutex> lock(mtx_);
		if (stopped_) return;

		stopped_ = true;
		running_ = false;

		// Stop watchdog
		{
			try { watchdogTimer_.cancel(); } catch (...) {}
			try { updateTimer_.cancel(); } catch (...) {}
		}

		// Stop all ZMQ clients
		if (zmqClient_) {
			try { zmqClient_->stop(); }
			catch (...) {}
		}
		for (auto& c : zmqClients_) {
			if (c) { try { c->stop(); } catch (...) {} }
		}
		zmqClients_.clear();

		zmqConnected_ = false;

		try { pollTimer_.cancel(); } catch (const std::exception& e) {
			mkpool::Logger::warn("[Generator] Error canceling poll timer: {}", e.what());
		}
		mkpool::Logger::info("[Generator] Stopped.");
	}

	void Generator::setBlockTemplateCallback(BlockTemplateCallback callback)
	{
		std::lock_guard<std::mutex> lock(mtx_);
		templateCallback_ = std::move(callback);
	}

	void Generator::setBlockNotifyCallback(std::function<void()> callback)
	{
		std::lock_guard<std::mutex> lock(mtx_);
		blockNotifyCallback_ = std::move(callback);
	}

	nlohmann::json Generator::currentBlockTemplate() const
	{
		std::lock_guard<std::mutex> lock(mtx_);
		return currentTemplate_;
	}

	bool Generator::startZmqClients()
	{
		// Build endpoint -> topics map. Returns true if at least one client was
		// created and started. Does NOT start the poll/watchdog (callers do that).
		std::unordered_map<std::string, std::vector<std::string>> endpointTopics;
		{
			std::lock_guard<std::mutex> lock(mtx_);
			// We deliberately do NOT subscribe to rawtx: it floods the process with
			// every mempool tx and has triggered libzmq signaler aborts when the
			// receive loop runs hot. hashblock is the new-block trigger; rawblock is
			// kept for future block-payload processing but is low-rate.
			for (const auto& ep : zmqEndpointsHashblock_) endpointTopics[ep].push_back("hashblock");
			for (const auto& ep : zmqEndpointsRawblock_)  endpointTopics[ep].push_back("rawblock");
			(void)zmqEndpointsRawtx_; // intentionally unused
		}

		if (!endpointTopics.empty()) {
			try {
				zmqClients_.reserve(endpointTopics.size());
				for (auto& kv : endpointTopics) {
					const std::string& endpoint = kv.first;
					const auto& topics = kv.second;
					auto client = std::make_unique<ZMQClient>(io_context_, endpoint);
					for (const auto& t : topics) client->subscribe(t);
					client->start([this](const std::string& topic, const std::string& data) {
						lastZmqEvent_ = std::chrono::steady_clock::now();
						handleZMQMessage(topic, data);
					});
					mkpool::Logger::info("[Generator] ZMQ client started: {} topics={}", endpoint, fmt::format("{}", fmt::join(topics, ",")));
					zmqClients_.push_back(std::move(client));
				}
				zmqConnected_ = true;
				mkpool::Logger::info("[Generator] ZMQ multi-endpoint listeners started ({} endpoints).", zmqClients_.size());
				return true;
			}
			catch (const std::exception& e) {
				mkpool::Logger::error("[Generator] Failed to initialize multi-endpoint ZMQ: {}.", e.what());
				zmqClients_.clear();
				return false;
			}
		}

		// Legacy single-endpoint path
		if (!zmqClient_) {
			mkpool::Logger::error("[Generator] ZMQ client not initialized.");
			return false;
		}
		zmqClient_->subscribe("hashblock");
		zmqClient_->subscribe("rawblock");
		zmqClient_->start([this](const std::string& topic, const std::string& data) {
			lastZmqEvent_ = std::chrono::steady_clock::now();
			handleZMQMessage(topic, data);
		});
		zmqConnected_ = true;
		mkpool::Logger::info("[Generator] ZMQ legacy listener started.");
		return true;
	}

	void Generator::startZMQ()
	{
		if (!startZmqClients()) {
			mkpool::Logger::warn("[Generator] ZMQ unavailable at startup; using RPC poll (watchdog will recover ZMQ when it appears).");
			useZMQ_ = false;
			startRPCPolling();
			scheduleZmqWatchdog();
			return;
		}

		// Fetch initial template; subsequent updates driven by ZMQ hashblock.
		pollBlockTemplate();
		// Watchdog detects a dead ZMQ feed (tip moved without a ZMQ msg) and reconnects.
		scheduleZmqWatchdog();
	}

	void Generator::startRPCPolling()
	{
		useZMQ_ = false;
		mkpool::Logger::info("[Generator] Using RPC polling mode");
		pollBlockTemplate();
	}

	void Generator::handleZMQMessage(const std::string& topic, const std::string& data)
	{
		if (topic == "hashblock") {
			// Decode block hash (binary -> hex)
			const std::string blockHashHex = mkpool::utils::to_hex(data);
			mkpool::Logger::info("[ZMQ] hashblock: {}", blockHashHex);
			mkpool::Logger::info("[Generator] New block via ZMQ, fetching template...");
			pollBlockTemplate();
		}
		else if (topic == "rawblock") {
			mkpool::Logger::debug("[ZMQ] rawblock received ({} bytes)", data.size());
			// Process later when needed
		}
		else if (topic == "rawtx") {
			mkpool::Logger::debug("[ZMQ] rawtx received ({} bytes)", data.size());
			// Process later when needed
		}
		else {
			mkpool::Logger::debug("[ZMQ] Unknown topic '{}' ({} bytes)", topic, data.size());
		}
	}

	void Generator::pollBlockTemplate()
	{
		bool local_running = false;
		std::shared_ptr<bitcoin::BitcoinClient> aux;
		{
			std::lock_guard<std::mutex> lock(mtx_);
			local_running = running_;
			aux = auxClient_;
		}
		if (!local_running)
			return;

		mkpool::Logger::debug("[Generator] Fetching block template via RPC...");
		bitcoinClient_->asyncGetBlockTemplate(
			[this, aux](const boost::system::error_code& ec, const nlohmann::json& response) {
				if (ec) {
					handleBlockTemplate(ec, response);
					return;
				}
				if (!aux) {
					handleBlockTemplate(ec, response);
					return;
				}
				// Self-assembled merge mining: pull a normal DOGE getblocktemplate
				// and merge it under result.aux. We build the DOGE coinbase/block
				// ourselves (per-miner payout), so we never call createauxblock and
				// never depend on the node's single-slot aux cache.
				aux->asyncGetBlockTemplate([this, parent_resp = response](const boost::system::error_code& aux_ec, const nlohmann::json& aux_resp) {
					if (aux_ec) {
						mkpool::Logger::error("[Generator] Error fetching aux (DOGE) block template: {}", aux_ec.message());
						handleBlockTemplate({}, parent_resp); // Fallback to parent block only
						return;
					}
					nlohmann::json merged = parent_resp;
					if (merged.contains("result") && merged["result"].is_object() &&
						aux_resp.contains("result") && aux_resp["result"].is_object()) {
						merged["result"]["aux"] = aux_resp["result"];
					}
					handleBlockTemplate({}, merged);
				});
			}
		);
	}

	void Generator::handleBlockTemplate(const boost::system::error_code& ec, const nlohmann::json& templateJson, bool fromPoll) {
		// If in ZMQ mode, reflect actual ZMQ connection state. Only the poll path
		// drives ZMQ-down detection / RPC-fallback; the keepalive fetch must not.
		if (fromPoll && useZMQ_) {
			bool anyConnected = false;
			if (!zmqClients_.empty()) {
				for (auto& c : zmqClients_) {
					if (c && c->isConnected()) { anyConnected = true; break; }
				}
			}
			else if (zmqClient_) {
				anyConnected = zmqClient_->isConnected();
			}
			if (!anyConnected) {
				if (zmqConnected_) {
					mkpool::Logger::warn("[Generator] ZMQ disconnected; switching to RPC polling");
				}
				zmqConnected_ = false;
				startRPCPolling();
				return;
			}
		}

		if (ec) {
			mkpool::Logger::error("[Generator] Error fetching block template: {}", ec.message());
			// In ZMQ mode keep heartbeat; RPC polling is only used when ZMQ down or silent too long.
		}
		else {
			std::lock_guard<std::mutex> lock(mtx_);

			// Safely extract new_prevhash
			std::string new_prevhash;
			if (templateJson.contains("result") && !templateJson["result"].is_null()) {
				new_prevhash = templateJson["result"].value("previousblockhash", "");
			}
			else {
				mkpool::Logger::warn("[Generator] No valid 'result' or 'previousblockhash' in templateJson");
				new_prevhash = "";
			}

			// Safely extract old_prevhash
			std::string old_prevhash;
			if (currentTemplate_.contains("result") && !currentTemplate_["result"].is_null()) {
				old_prevhash = currentTemplate_["result"].value("previousblockhash", "");
			}
			else {
				old_prevhash = "";
			}

			// Also track the aux (DOGE) prevhash: when the DOGE tip moves we must
			// rebuild work so the committed aux template stays current, even if the
			// parent (LTC) tip is unchanged.
			auto aux_prevhash_of = [](const nlohmann::json& tj) -> std::string {
				if (tj.contains("result") && tj["result"].is_object() &&
					tj["result"].contains("aux") && tj["result"]["aux"].is_object()) {
					return tj["result"]["aux"].value("previousblockhash", "");
				}
				return "";
			};
			std::string new_aux_prevhash = aux_prevhash_of(templateJson);
			std::string old_aux_prevhash = aux_prevhash_of(currentTemplate_);

			// Compare and update if either chain's tip changed.
			if (new_prevhash != old_prevhash || new_aux_prevhash != old_aux_prevhash) {
				currentTemplate_ = templateJson;
				lastTipChangeAt_ = std::chrono::steady_clock::now();
				mkpool::Logger::info("[Generator] New work (prevhash: {} aux: {})",
					new_prevhash.substr(0, 16),
					new_aux_prevhash.empty() ? std::string("-") : new_aux_prevhash.substr(0, 16));
				if (templateCallback_) {
					templateCallback_(templateJson);
				}
				// Fan out the new-block event to other in-process consumers
				// (e.g. NetworkStatsTracker) so they need not open their own
				// duplicate ZMQ subscription. Handler just posts work; safe here.
				if (blockNotifyCallback_) {
					blockNotifyCallback_();
				}
				// A real new block resets the keepalive clock; the next periodic
				// rebroadcast fires jobUpdateInterval_ after this job, never before.
				armJobUpdate();
			}
			else if (!fromPoll) {
				// jobUpdateInterval_: tip unchanged but the
				// interval elapsed. Refresh template (fresh curtime) and rebroadcast;
				// the Stratifier emits clean_jobs=false (prevhash unchanged) so miners
				// keep their work and roll onto the fresher job. Keepalive that stops
				// strict clients (NiceHash/Braiins/MRR/farm controllers) idle-timing-out.
				currentTemplate_ = templateJson;
				if (templateCallback_) {
					templateCallback_(templateJson);
				}
				mkpool::Logger::debug("[Generator] keepalive job rebroadcast ({}s)", jobUpdateInterval_.count());
			}
			else {
				mkpool::Logger::debug("[Generator] Block template unchanged.");
			}
		}

		if (!fromPoll) return; // keepalive rebroadcast done above; only poll reschedules
		// Schedule next tick:
		// - If ZMQ active -> slow heartbeat (pollInterval_ * 6)
		// - Else -> normal RPC polling interval
		auto nextInterval = (useZMQ_ && zmqConnected_) ? (pollInterval_ * 6) : pollInterval_;

		pollTimer_.expires_after(nextInterval);
		pollTimer_.async_wait([this](const boost::system::error_code& timer_ec) {
			if (!timer_ec && running_) {
				pollBlockTemplate();
			}
			else if (timer_ec) {
				if (timer_ec != boost::asio::error::operation_aborted) {
					mkpool::Logger::warn("[Generator] Poll timer error: {}", timer_ec.message());
				}
			}
			});
	}

	void Generator::armJobUpdate()
	{
		updateTimer_.expires_after(jobUpdateInterval_);
		updateTimer_.async_wait([this](const boost::system::error_code& ec) {
			// operation_aborted => a real new block (or prior arm) reset us; the
			// resetter installed a fresh wait, so bow out.
			if (ec || !running_) return;

			// Re-arm now so the keepalive cadence self-sustains even if this GBT
			// fetch fails; it is reset again on the next real new block.
			armJobUpdate();

			std::shared_ptr<bitcoin::BitcoinClient> aux;
			{
				std::lock_guard<std::mutex> lock(mtx_);
				if (!running_) return;
				aux = auxClient_;
			}
			// Same fetch (+aux merge) as pollBlockTemplate, routed through the
			// keepalive path (fromPoll=false) so handleBlockTemplate rebroadcasts
			// even when the tip is unchanged.
			bitcoinClient_->asyncGetBlockTemplate(
				[this, aux](const boost::system::error_code& gec, const nlohmann::json& response) {
					if (gec || !aux) { handleBlockTemplate(gec, response, false); return; }
					aux->asyncGetBlockTemplate(
						[this, parent = response](const boost::system::error_code& aec, const nlohmann::json& aresp) {
							if (aec) { handleBlockTemplate({}, parent, false); return; }
							nlohmann::json merged = parent;
							if (merged.contains("result") && merged["result"].is_object() &&
								aresp.contains("result") && aresp["result"].is_object())
								merged["result"]["aux"] = aresp["result"];
							handleBlockTemplate({}, merged, false);
						});
				});
		});
	}

	void Generator::submitBlock(const std::string& blockData, BlockSubmissionCallback callback)
	{
		mkpool::Logger::info("[Generator] Submitting block...");
		bitcoinClient_->asyncSubmitBlock(
			blockData,
			[callback](const boost::system::error_code& ec, const nlohmann::json& response) {
				if (ec) {
					mkpool::Logger::error("[Generator] Error submitting block: {}", ec.message());
				}
				callback(response);
			}
		);
	}

	void Generator::scheduleZmqWatchdog()
	{
		// Liveness by CONTENT, not elapsed time. A chain may legitimately produce
		// no block for over an hour (BTC/BCH), so silence is never treated as a
		// failure. The heartbeat RPC poll observes every tip change even when ZMQ
		// is down; if a tip change occurred with no accompanying ZMQ message, the
		// feed is dead and we reconnect it. Block-time agnostic, self-healing.
		watchdogTimer_.expires_after(pollInterval_);
		watchdogTimer_.async_wait([this](const boost::system::error_code& ec) {
			if (ec || !running_) return;
			const auto now = std::chrono::steady_clock::now();

			bool needRecover;
			if (useZMQ_) {
				const bool haveTipChange = lastTipChangeAt_.time_since_epoch().count() != 0;
				needRecover = haveTipChange && lastTipChangeAt_ > lastZmqEvent_ + zmqRecoverGrace_;
			}
			else {
				// Watchdog only runs when ZMQ was requested; a false useZMQ_ means we
				// fell back to RPC and should keep trying to bring ZMQ back up.
				needRecover = true;
			}

			if (needRecover && (now - lastZmqRecover_) > zmqRecoverThrottle_) {
				lastZmqRecover_ = now;
				if (useZMQ_) {
					const auto gap = std::chrono::duration_cast<std::chrono::seconds>(
						lastTipChangeAt_ - lastZmqEvent_).count();
					mkpool::Logger::warn("[Generator] ZMQ feed stale: tip advanced {}s after last ZMQ event; reconnecting.", gap);
					reconnectZmq();
				}
				else if (reconnectZmq()) {
					useZMQ_ = true;
					mkpool::Logger::info("[Generator] Returned to ZMQ-primary mode.");
				}
			}

			if (running_) scheduleZmqWatchdog();
		});
	}

	bool Generator::reconnectZmq()
	{
		// Single-threaded io_context (main_io_): safe to tear down and rebuild the
		// client list without locks. stop() is idempotent and releases the ZMQ fd
		// without closing it (see ZMQClient::stop).
		for (auto& c : zmqClients_) { if (c) c->stop(); }
		zmqClients_.clear();
		zmqConnected_ = false;

		if (startZmqClients()) {
			// Treat reconnect as fresh activity so resubscribed sockets get time
			// to deliver before staleness is re-evaluated.
			lastZmqEvent_ = std::chrono::steady_clock::now();
			mkpool::Logger::info("[Generator] ZMQ (re)connected; resubscribed and listening.");
			pollBlockTemplate();
			return true;
		}
		mkpool::Logger::warn("[Generator] ZMQ (re)connect failed; heartbeat RPC poll continues, will retry.");
		return false;
	}

	void Generator::setAuxClient(std::shared_ptr<bitcoin::BitcoinClient> client, const std::string& payoutAddress)
	{
		std::lock_guard<std::mutex> lock(mtx_);
		auxClient_ = std::move(client);
		auxPayoutAddress_ = payoutAddress;
	}

} // namespace mkpool
