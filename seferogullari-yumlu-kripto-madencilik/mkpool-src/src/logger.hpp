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
// File:        logger.hpp
// Description: Logging setup and helpers (spdlog).
// Created:     2026-06-02
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async.h>

#include <memory>
#include <string>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "config.hpp"

namespace mkpool {
	struct Config;

	class Logger {
	public:
		Logger(const Logger&) = delete;
		Logger& operator=(const Logger&) = delete;
		Logger(Logger&&) = delete;
		Logger& operator=(Logger&&) = delete;

		static void init(const Config& config, const std::string& instanceName = "") {
			if (instance_) {
				return;
			}

			try {
				// Ensure log directory exists
				std::filesystem::create_directories(config.global.logPath);

				// Per-instance, fixed-name rotating file. Each pool process runs a
				// single coin/network (config-<coin>-<net>.json), so the file MUST
				// be named per instance - otherwise every coin's process writes the
				// same file and they corrupt each other's rotation. A fixed name
				// (not date-based) lets spdlog's size rotation actually bound disk
				// usage; date-based names accumulated forever.
				std::string base = instanceName.empty()
					? std::string("mkpool")
					: (std::string("mkpool-") + instanceName);
				std::string log_filename = base + ".log";
				auto log_file = std::filesystem::path(config.global.logPath) / log_filename;

				// Create sinks
				std::vector<spdlog::sink_ptr> sinks;

				// Rotating file sink: 5MB x 5 files = 25MB hard cap per instance.
				auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file.string(), 5 * 1024 * 1024, 5);
				sinks.push_back(file_sink);

				// Console sink -> stdout (captured by journald). Use automatic color
				// so it emits ANSI only on a real TTY, keeping journald/file clean.
				auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
				console_sink->set_color_mode(spdlog::color_mode::automatic);
				sinks.push_back(console_sink);

				// Use async logging
				spdlog::init_thread_pool(8192, 1);
				auto logger = std::make_shared<spdlog::async_logger>(
					"mkpool", sinks.begin(), sinks.end(), spdlog::thread_pool(),
					spdlog::async_overflow_policy::block);

				// Map Config logLevel
				switch (config.global.logLevel) {
				case 0: logger->set_level(spdlog::level::off); break;
				case 1: logger->set_level(spdlog::level::err); break;
				case 2: logger->set_level(spdlog::level::info); break;
				case 3: logger->set_level(spdlog::level::debug); break;
				default: logger->set_level(spdlog::level::info); break;
				}

				logger->flush_on(spdlog::level::err);

				spdlog::register_logger(logger);
				spdlog::set_default_logger(logger);

				instance_ = std::move(logger);
			}
			catch (const std::exception& e) {
				std::cerr << "Logger init failed: " << e.what() << "\n";
				throw;
			}
		}

		template<typename... Args>
		static void debug(const char* fmt, Args&&... args) {
			log(spdlog::level::debug, fmt, std::forward<Args>(args)...);
		}

		template<typename... Args>
		static void info(const char* fmt, Args&&... args) {
			log(spdlog::level::info, fmt, std::forward<Args>(args)...);
		}

		template<typename... Args>
		static void warn(const char* fmt, Args&&... args) {
			log(spdlog::level::warn, fmt, std::forward<Args>(args)...);
		}

		template<typename... Args>
		static void error(const char* fmt, Args&&... args) {
			log(spdlog::level::err, fmt, std::forward<Args>(args)...);
		}

		template<typename... Args>
		static void critical(const char* fmt, Args&&... args) {
			log(spdlog::level::critical, fmt, std::forward<Args>(args)...);
		}

		static void shutdown() {
			if (instance_) {
				instance_->flush();
				spdlog::shutdown();
				instance_.reset();
			}
		}

	private:
		static inline std::shared_ptr<spdlog::async_logger> instance_{ nullptr };

		template<typename... Args>
		static void log(spdlog::level::level_enum level, const char* fmt, Args&&... args) {
			if (!instance_) {
				throw std::runtime_error("Logger not initialized");
			}
			instance_->log(level, spdlog::fmt_lib::runtime(fmt), std::forward<Args>(args)...);
		}

		Logger() = default;
	};
} // namespace mkpool