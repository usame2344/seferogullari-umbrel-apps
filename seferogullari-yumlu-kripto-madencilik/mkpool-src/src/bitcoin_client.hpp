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
// File:        bitcoin_client.hpp
// Description: Bitcoin-family JSON-RPC client interface and block-template types.
// Created:     2025-03-30
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool

#pragma once

#include "config.hpp"
#include "beast_client.hpp"
#include <boost/asio.hpp>
#include <boost/multiprecision/cpp_int.hpp>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <functional>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string_view>
#include <cmath>
#include <optional>
#include <atomic>
#include <vector>
#include <boost/asio/steady_timer.hpp>

namespace mkpool
{
    namespace bitcoin 
    {
        /// \brief Bitcoin Address Types
        /// 
        /// Legacy (P2PKH), Native SegWit (P2WPKH), Nested SegWit (P2SH) and Taproot (P2TR)
        /// 
        /// Last revision: 28/11/2025
        enum class BitcoinAddressType
        {
            /// \brief Pay-to-Public-Key-Hash (P2PKH) (192 vbytes)
            /// Example: 12rYgz414HBXdhhK72BkR9VHZSU23dqqG7
            Legacy,

            /// \brief Pay-to-Witness-Public-Key-Hash (P2WPKH) (110 vbytes)
            /// Example: bc1qhrnucvul769yld6q09m8skwkp6zrecxhc00jcw
            Segwit,

            /// \brief Pay-to-Script-Hash (P2SH), P2SH-P2WPKH, P2SH-P2WSH (134 vbytes)
            /// Example: 3BY19nUKCAkrnzrgRezJoekGv4AFzsTs2z
            NestedSegwit,

            /// \brief Pay-to-Taproot (P2TR) (112 vbytes)
            /// Example: bc1p5d7rjq7g6r4jdyhzks9smlaqtedr4dekq08ge8ztwac72sfr9rusxg3297
            Taproot,
        };

        /// \brief Bitcoin Transaction Categories
        enum class BitcoinTransactionCategory {

            /// \brief Transactions sent.
            Send,

            /// \brief Non-coinbase transactions received.
            Receive,

            /// \brief Coinbase transactions received with more than 100 confirmations.
            Generate,

            /// \brief Coinbase transactions received with 100 or fewer confirmations.
            Immature,

            /// \brief Orphaned coinbase transactions received.
            Orphan,
        };


        /// \brief Convert BitcoinTransactionCategory to string.
        constexpr std::string_view BitcoinTransactionToString(BitcoinTransactionCategory c) noexcept {
            switch (c) {
            case BitcoinTransactionCategory::Send:     return "send";
            case BitcoinTransactionCategory::Receive:  return "receive";
            case BitcoinTransactionCategory::Generate: return "generate";
            case BitcoinTransactionCategory::Immature: return "immature";
            case BitcoinTransactionCategory::Orphan:   return "orphan";
            }
            return "";
        }

        /// \brief Convert string to BitcoinTransactionCategory.
        constexpr std::optional<BitcoinTransactionCategory>
            BitcoinTransactionFromString(std::string_view s) noexcept {
            if (s == "send")     return BitcoinTransactionCategory::Send;
            if (s == "receive")  return BitcoinTransactionCategory::Receive;
            if (s == "generate") return BitcoinTransactionCategory::Generate;
            if (s == "immature") return BitcoinTransactionCategory::Immature;
            if (s == "orphan")   return BitcoinTransactionCategory::Orphan;
            return std::nullopt;
        }

        /// \brief Bitcoin transaction status classification.
        enum class BitcoinTransactionStatus
        {
            /// \brief Have 6+ confirmations (normal tx) or fully mature (mined tx).
            Confirmed,

            /// \brief Normal (sent/received) transactions not yet mined into a block.
            Unconfirmed,

            /// \brief Confirmed, but waiting for the recommended number of confirmations.
            Confirming,

            /// \brief Conflicts with another transaction or mempool entry.
            Conflicted,

            /// \brief Abandoned by the wallet.
            Abandoned,

            /// \brief Generated (mined) transaction waiting for maturity.
            Immature,

            /// \brief Mined but not accepted to the chain.
            NotAccepted
        };

        /// \brief Bitcoin JSON-RPC error codes used by bitcoind.
        enum class BitcoinRPCErrorCode : int
        {
            /// \brief Invalid JSON-RPC request (mapped to HTTP 400).
            RPC_INVALID_REQUEST = -32600,

            /// \brief Method not found (mapped to HTTP 404).
            RPC_METHOD_NOT_FOUND = -32601,

            /// \brief Invalid JSON-RPC parameters.
            RPC_INVALID_PARAMS = -32602,

            /// \brief Internal error inside bitcoind (e.g., datadir corruption).
            RPC_INTERNAL_ERROR = -32603,

            /// \brief JSON parse error.
            RPC_PARSE_ERROR = -32700,


            // ------------------------------------------------------------
            // General application errors
            // ------------------------------------------------------------

            /// \brief std::exception thrown during command handling.
            RPC_MISC_ERROR = -1,

            /// \brief Unexpected parameter type.
            RPC_TYPE_ERROR = -3,

            /// \brief Invalid address or key.
            RPC_INVALID_ADDRESS_OR_KEY = -5,

            /// \brief Out of memory during operation.
            RPC_OUT_OF_MEMORY = -7,

            /// \brief Invalid, missing, or duplicate parameter.
            RPC_INVALID_PARAMETER = -8,

            /// \brief Database-related error.
            RPC_DATABASE_ERROR = -20,

            /// \brief Deserialization or raw structure validation error.
            RPC_DESERIALIZATION_ERROR = -22,

            /// \brief Generic transaction/block verification error.
            RPC_VERIFY_ERROR = -25,

            /// \brief Transaction or block rejected by consensus rules.
            RPC_VERIFY_REJECTED = -26,

            /// \brief Transaction already exists in the UTXO set.
            RPC_VERIFY_ALREADY_IN_UTXO_SET = -27,

            /// \brief Node is still warming up.
            RPC_IN_WARMUP = -28,

            /// \brief RPC method is deprecated.
            RPC_METHOD_DEPRECATED = -32,


            // ------------------------------------------------------------
            // Backwards compatibility aliases
            // ------------------------------------------------------------

            /// \brief Alias for RPC_VERIFY_ERROR.
            RPC_TRANSACTION_ERROR = RPC_VERIFY_ERROR,

            /// \brief Alias for RPC_VERIFY_REJECTED.
            RPC_TRANSACTION_REJECTED = RPC_VERIFY_REJECTED,


            // ------------------------------------------------------------
            // P2P client errors
            // ------------------------------------------------------------

            /// \brief Bitcoin node is not connected.
            RPC_CLIENT_NOT_CONNECTED = -9,

            /// \brief Initial block download still in progress.
            RPC_CLIENT_IN_INITIAL_DOWNLOAD = -10,

            /// \brief Node already added.
            RPC_CLIENT_NODE_ALREADY_ADDED = -23,

            /// \brief Node was not previously added.
            RPC_CLIENT_NODE_NOT_ADDED = -24,

            /// \brief Node to disconnect not found among connected peers.
            RPC_CLIENT_NODE_NOT_CONNECTED = -29,

            /// \brief Invalid IP or subnet specification.
            RPC_CLIENT_INVALID_IP_OR_SUBNET = -30,

            /// \brief Connection manager instance could not be found.
            RPC_CLIENT_P2P_DISABLED = -31,

            /// \brief Maximum outbound/block-relay connections already reached.
            RPC_CLIENT_NODE_CAPACITY_REACHED = -34,


            // ------------------------------------------------------------
            // Chain & mempool errors
            // ------------------------------------------------------------

            /// \brief Mempool is disabled or unavailable.
            RPC_CLIENT_MEMPOOL_DISABLED = -33,


            // ------------------------------------------------------------
            // Wallet errors
            // ------------------------------------------------------------

            /// \brief Unspecified wallet error (missing key, etc.).
            RPC_WALLET_ERROR = -4,

            /// \brief Insufficient wallet funds.
            RPC_WALLET_INSUFFICIENT_FUNDS = -6,

            /// \brief Invalid label name.
            RPC_WALLET_INVALID_LABEL_NAME = -11,

            /// \brief Keypool depleted; requires keypoolrefill.
            RPC_WALLET_KEYPOOL_RAN_OUT = -12,

            /// \brief Wallet must be unlocked (walletpassphrase required).
            RPC_WALLET_UNLOCK_NEEDED = -13,

            /// \brief Incorrect wallet passphrase.
            RPC_WALLET_PASSPHRASE_INCORRECT = -14,

            /// \brief Invalid wallet encryption state for the requested action.
            RPC_WALLET_WRONG_ENC_STATE = -15,

            /// \brief Failed to encrypt the wallet.
            RPC_WALLET_ENCRYPTION_FAILED = -16,

            /// \brief Wallet is already unlocked.
            RPC_WALLET_ALREADY_UNLOCKED = -17,

            /// \brief The specified wallet could not be found.
            RPC_WALLET_NOT_FOUND = -18,

            /// \brief No wallet specified while multiple wallets are loaded.
            RPC_WALLET_NOT_SPECIFIED = -19,

            /// \brief The same wallet is already loaded.
            RPC_WALLET_ALREADY_LOADED = -35,

            /// \brief A wallet with this name already exists.
            RPC_WALLET_ALREADY_EXISTS = -36,

            /// \brief Backwards-compatible alias for RPC_WALLET_INVALID_LABEL_NAME.
            RPC_WALLET_INVALID_ACCOUNT_NAME = RPC_WALLET_INVALID_LABEL_NAME,


            // ------------------------------------------------------------
            // Deprecated / reserved
            // ------------------------------------------------------------

            /// \brief Server is in safe mode; command is not allowed.
            RPC_FORBIDDEN_BY_SAFE_MODE = -2
        };

        /// \brief Bitcoin JSON-RPC command names used by bitcoind.
        namespace BitcoinCommands
        {
            //
            // ────────────────────────────────────────────────────────────────
            // Blockchain information
            // ────────────────────────────────────────────────────────────────
            //

            /// \brief getblockchaininfo - Returns blockchain state and statistics.
            inline constexpr std::string_view GetBlockchainInfo = "getblockchaininfo";

            /// \brief getblockcount - Returns the height of the best (tip) block.
            inline constexpr std::string_view GetBlockCount = "getblockcount";

            /// \brief getbestblockhash - Returns the hash of the best (tip) block.
            inline constexpr std::string_view GetBestBlockHash = "getbestblockhash";

            /// \brief getdifficulty - Returns current proof-of-work difficulty.
            inline constexpr std::string_view GetDifficulty = "getdifficulty";


            //
            // ────────────────────────────────────────────────────────────────
            // Block retrieval & manipulation
            // ────────────────────────────────────────────────────────────────
            //

            /// \brief getblockhash - Returns the block hash at a specified height.
            inline constexpr std::string_view GetBlockHash = "getblockhash";

            /// \brief getblock - Retrieves a block by hash.
            inline constexpr std::string_view GetBlock = "getblock";

            /// \brief getblocktemplate - Returns a block template for mining.
            inline constexpr std::string_view GetBlockTemplate = "getblocktemplate";

            /// \brief getblocksubsidy - Returns block subsidy details.
            inline constexpr std::string_view GetBlockSubsidy = "getblocksubsidy";

            /// \brief submitblock - Submits a block to the network.
            inline constexpr std::string_view SubmitBlock = "submitblock";

            /// \brief preciousblock - Marks a block as having higher priority for reorgs.
            inline constexpr std::string_view PreciousBlock = "preciousblock";


            //
            // ────────────────────────────────────────────────────────────────
            // Mining
            // ────────────────────────────────────────────────────────────────
            //

            /// \brief getmininginfo - Returns mining statistics and solver state.
            inline constexpr std::string_view GetMiningInfo = "getmininginfo";

            /// \brief getnetworkhashps - Returns estimated network hash rate.
            inline constexpr std::string_view GetNetworkHashPS = "getnetworkhashps";


            //
            // ────────────────────────────────────────────────────────────────
            // Transactions
            // ────────────────────────────────────────────────────────────────
            //

            /// \brief decoderawtransaction - Decodes a serialized raw transaction hex.
            inline constexpr std::string_view DecodeRawTransaction = "decoderawtransaction";

            /// \brief getrawtransaction - Returns raw transaction data by txid.
            inline constexpr std::string_view GetRawTransaction = "getrawtransaction";

            /// \brief gettransaction - Returns details of a wallet transaction.
            inline constexpr std::string_view GetTransaction = "gettransaction";

            /// \brief submittransaction - Submits a raw transaction to the mempool.
            inline constexpr std::string_view SubmitTransaction = "submittransaction";


            //
            // ────────────────────────────────────────────────────────────────
            // Wallet operations
            // ────────────────────────────────────────────────────────────────
            //

            /// \brief getbalance - Returns the wallet balance.
            inline constexpr std::string_view GetBalance = "getbalance";

            /// \brief sendmany - Sends assets to multiple recipients.
            inline constexpr std::string_view SendMany = "sendmany";

            /// \brief sendtoaddress - Sends funds to a single address.
            inline constexpr std::string_view SendToAddress = "sendtoaddress";

            /// \brief walletpassphrase - Unlocks an encrypted wallet.
            inline constexpr std::string_view WalletPassphrase = "walletpassphrase";

            /// \brief walletlock - Re-locks an encrypted wallet.
            inline constexpr std::string_view WalletLock = "walletlock";


            //
            // ────────────────────────────────────────────────────────────────
            // Address validation & metadata
            // ────────────────────────────────────────────────────────────────
            //

            /// \brief validateaddress - Checks if an address is valid.
            inline constexpr std::string_view ValidateAddress = "validateaddress";

            /// \brief getaddressinfo - Returns extended address metadata.
            inline constexpr std::string_view GetAddressInfo = "getaddressinfo";

            /// \brief listunspent - Lists UTXOs available for spending.
            inline constexpr std::string_view ListUnspent = "listunspent";


            //
            // ────────────────────────────────────────────────────────────────
            // Network & peer information
            // ────────────────────────────────────────────────────────────────
            //

            /// \brief getnetworkinfo - Network and protocol information.
            inline constexpr std::string_view GetNetworkInfo = "getnetworkinfo";

            /// \brief getpeerinfo - Returns statistics about connected peers.
            inline constexpr std::string_view GetPeerInfo = "getpeerinfo";

            /// \brief getconnectioncount - Returns number of connected peers.
            inline constexpr std::string_view GetConnectionCount = "getconnectioncount";


            //
            // ────────────────────────────────────────────────────────────────
            // Legacy
            // ────────────────────────────────────────────────────────────────
            //

            /// \brief getinfo - Legacy multi-purpose info command.
            inline constexpr std::string_view GetInfo = "getinfo";
        }

        class BitcoinClient : public std::enable_shared_from_this<BitcoinClient>
        {
            public:
                using json = nlohmann::json;
                using RPCCallback = std::function<void(const boost::system::error_code&, const json&)>;

                BitcoinClient(boost::asio::io_context& io_context,
                    const std::string& host,
                    const std::string& port,
                    const std::string& rpcUser = "",
                    const std::string& rpcPassword = "",
                    bool bchMode = false,
                    ChainKind chainKind = ChainKind::Bitcoin);

                /// True if this client talks to a BCH node (changes GBT rules).
                [[nodiscard]] bool isBchMode() const noexcept { return bchMode_; }

                void asyncValidateAddress(const std::string& address, std::function<void(const boost::system::error_code&, bool valid, bool script, bool segwit)> callback);
                void asyncValidateTxn(const std::string& txn, RPCCallback callback);
                void asyncGetBlockTemplate(RPCCallback callback);
                void asyncGetBlockCount(RPCCallback callback);
                void asyncGetBlockHash(int height, RPCCallback callback);
                void asyncGetBestBlockHash(RPCCallback callback);
                void asyncSubmitBlock(const std::string& blockData, RPCCallback callback);
                void asyncGetAuxBlock(const std::string& payoutAddress, RPCCallback callback);
                void asyncSubmitAuxBlock(const std::string& hash, const std::string& auxpowHex, RPCCallback callback);
                void asyncPreciousBlock(const std::string& blockData, RPCCallback callback);
                void asyncSubmitTxn(const std::string& txnData, RPCCallback callback);
                void asyncGetTxn(const std::string& txHash, RPCCallback callback);
                void asyncGetMiningInfo(RPCCallback callback);
                void asyncGetBalance(RPCCallback callback);
                void asyncGetAddressInfo(const std::string& address, RPCCallback callback);
                void asyncListUnspent(int minConf, int maxConf, const std::vector<std::string>& addresses, RPCCallback callback);
                void asyncGetBlockchainInfo(RPCCallback callback);
                void asyncGetBlock(const std::string& hash, RPCCallback callback);
                void asyncGetNetworkInfo(RPCCallback callback);
                void asyncGetPeerInfo(RPCCallback callback);
                void asyncGetConnectionCount(RPCCallback callback);
                void asyncGetInfo(RPCCallback callback);

                // --- Multi-node failover -----------------------------------
                // Register an additional bitcoind the client fails over to when
                // the current node stops answering (transport failure). Order of
                // registration is the failover order after the primary. Inert
                // unless at least one fallback is added: with a single node the
                // RPC path is byte-identical to the original single-endpoint code.
                void addFallbackNode(const std::string& host, const std::string& port,
                                     const std::string& rpcUser, const std::string& rpcPassword);

                // --- Redundant block propagation ---------------------------
                // Register an extra node that ALSO receives submitblock (fire and
                // forget) for faster/redundant propagation. Never gates or delays
                // the primary submit; failures here are logged and ignored.
                void addSubmitEndpoint(const std::string& host, const std::string& port,
                                       const std::string& rpcUser, const std::string& rpcPassword);

                // Start the primary-recovery watchdog. No-op when only one
                // node is configured. Must be called after fallbacks are added.
                void startWatchdog();

            private:
                // Runs `request` against the RPC layer. With a single configured
                // node this is one BeastClient attempt whose result is delivered
                // verbatim (identical to the historical behaviour). With >1 node
                // it fails over across nodes on transport failure (see .cpp).
                void asyncRPC(const json& request, RPCCallback callback);

                struct RpcEndpoint {
                    std::string host;
                    std::string port;
                    std::string authHeader;  // pre-encoded base64 of "user:pass"
                };
                static std::string encodeAuth(const std::string& user, const std::string& pass);
                void rpcTryEndpoint(std::shared_ptr<json> request,
                                    std::shared_ptr<RPCCallback> callback,
                                    std::size_t idx, std::size_t tried);
                void scheduleWatchdog();
                void broadcastSubmit(const json& request);

                std::vector<RpcEndpoint> endpoints_;      // [0] = primary, then fallbacks
                std::atomic<std::size_t> current_{0};     // last-known-good endpoint index
                std::vector<RpcEndpoint> submitEndpoints_; // extra redundant submit targets
                std::unique_ptr<boost::asio::steady_timer> watchdog_;

                // --- Block-submission retry --------------------------------
                // submitblock / submitauxblock are the only RPCs where a transient
                // transport failure can cost us a found block. asyncSubmitWithRetry
                // re-issues the request ONLY when the node returned no valid
                // response (ec set); any genuine node response (clean accept OR a
                // rejection/soft result) is delivered to the caller unchanged, so
                // normal submission behaviour is byte-identical to before. The
                // caller's callback is always invoked exactly once.
                struct SubmitRetryState;
                void submitAttempt(const std::shared_ptr<SubmitRetryState>& st);
                void asyncSubmitWithRetry(json request, RPCCallback callback);

                boost::asio::io_context& io_context_;
                boost::asio::ip::tcp::socket socket_;
                std::string host_;
                std::string port_;
                std::string authHeader_;
                boost::asio::streambuf streambuf_;
                bool bchMode_{false};
                ChainKind chainKind_{ChainKind::Bitcoin};
        };
    } // namespace bitcoin
} // namespace mkpool

