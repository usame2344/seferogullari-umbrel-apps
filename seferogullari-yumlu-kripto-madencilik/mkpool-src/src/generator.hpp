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
// File:        generator.hpp
// Description: Block-template generator interface.
// Created:     2025-04-08
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include "bitcoin_client.hpp"
#include "zmq_client.hpp"
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <chrono>

namespace mkpool {

	using BlockTemplateCallback = std::function<void(const nlohmann::json& blockTemplate)>;
	using BlockSubmissionCallback = std::function<void(const nlohmann::json& response)>;

	class Generator {
	public:
		Generator(boost::asio::io_context& io_context,
			std::shared_ptr<bitcoin::BitcoinClient> bitcoinClient,
			bool useZMQ = false,
			std::chrono::seconds pollInterval = std::chrono::seconds(10));

		~Generator();

		void start();
		void stop();
		void setBlockTemplateCallback(BlockTemplateCallback callback);
		// Invoked (on the generator's io_context) whenever the chain tip changes,
		// so other in-process consumers (e.g. NetworkStatsTracker) can react to a
		// new block without opening their own ZMQ subscription.
		void setBlockNotifyCallback(std::function<void()> callback);
		void submitBlock(const std::string& blockData, BlockSubmissionCallback callback);
		nlohmann::json currentBlockTemplate() const;
		void setAuxClient(std::shared_ptr<bitcoin::BitcoinClient> client, const std::string& payoutAddress);

		// Configure multi-endpoint ZMQ (per-topic). Call before start().
		// Empty vectors -> legacy single endpoint (zmqAddress) is used.
		void setZmqEndpoints(const std::vector<std::string>& hashblock,
			const std::vector<std::string>& rawblock,
			const std::vector<std::string>& rawtx)
		{
			std::lock_guard<std::mutex> lock(mtx_);
			zmqEndpointsHashblock_ = hashblock;
			zmqEndpointsRawblock_ = rawblock;
			zmqEndpointsRawtx_ = rawtx;
		}

	private:
		void pollBlockTemplate();
		void handleBlockTemplate(const boost::system::error_code& ec, const nlohmann::json& templateJson, bool fromPoll = true);
		// periodic job rebroadcast (update_base @ update_interval).
		// Every jobUpdateInterval_ of silence we re-fetch GBT and rebroadcast the
		// job with clean_jobs=false, so strict clients (NiceHash/Braiins/MRR/farm
		// controllers) don't hit their job-idle timeout. Re-armed on every real
		// new-block broadcast so it never fires within an interval of a real job.
		void armJobUpdate();
		void handleZMQMessage(const std::string& topic, const std::string& data);
		void startZMQ();
		void startRPCPolling();

		// ZMQ watchdog to fallback when silent/errored
		void scheduleZmqWatchdog();
		bool startZmqClients();
		bool reconnectZmq();

		boost::asio::io_context& io_context_;
		std::shared_ptr<bitcoin::BitcoinClient> bitcoinClient_;
		std::unique_ptr<ZMQClient> zmqClient_; // legacy single endpoint
		std::vector<std::unique_ptr<ZMQClient>> zmqClients_; // multi-endpoint
		boost::asio::steady_timer pollTimer_;
		boost::asio::steady_timer watchdogTimer_{ io_context_ };
		boost::asio::steady_timer updateTimer_{ io_context_ }; // 30s job rebroadcast
		std::chrono::seconds pollInterval_;
		std::chrono::seconds jobUpdateInterval_{ std::chrono::seconds(30) };

		mutable std::mutex mtx_;
		nlohmann::json currentTemplate_;
		BlockTemplateCallback templateCallback_;
		std::function<void()> blockNotifyCallback_;
		bool running_;
		bool useZMQ_;
		bool zmqConnected_;
		bool stopped_{ false };

		// Multi-endpoint ZMQ configuration
		std::vector<std::string> zmqEndpointsHashblock_;
		std::vector<std::string> zmqEndpointsRawblock_;
		std::vector<std::string> zmqEndpointsRawtx_;

		// ZMQ monitoring state
		std::chrono::steady_clock::time_point lastZmqEvent_{};
		std::chrono::seconds fallbackTimeout_{ std::chrono::seconds(120) }; // dynamic at start
		std::chrono::steady_clock::time_point lastTipChangeAt_{};
		std::chrono::steady_clock::time_point lastZmqRecover_{};
		std::chrono::seconds zmqRecoverGrace_{ std::chrono::seconds(20) };
		std::chrono::seconds zmqRecoverThrottle_{ std::chrono::seconds(30) };

		// Aux merged mining
		std::shared_ptr<bitcoin::BitcoinClient> auxClient_{nullptr};
		std::string auxPayoutAddress_;
	};

	using GeneratorPtr = std::shared_ptr<Generator>;

} // namespace mkpool
