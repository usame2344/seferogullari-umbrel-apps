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
// File:        replay_main.cpp
// Description: A libFuzzer-free driver that replays a corpus through a fuzz
//              target's LLVMFuzzerTestOneInput().
// Created:     2026-07-16
// Project:     mkpool - Modern Multi-Coin Solo Mining Pool
//
// WHY THIS EXISTS
// ---------------
// Coverage-guided fuzzing needs clang's libFuzzer, and clang's libstdc++
// autodetection on the CI base image is not something we can pin reliably. So
// CI does not fuzz -- it REPLAYS. Each fuzz target is linked against this main
// instead of libFuzzer and run over the committed corpus (which includes the
// literal 2026-07-15 attack payload) under ASan/UBSan, using the same g++ the
// rest of the build already passes with.
//
// That keeps the regression guarantee -- a reintroduced bug crashes on a corpus
// seed -- without any clang dependency. For actual exploration (mutating inputs
// to find NEW paths), build a target with clang locally; see tests/fuzz/README.md.
//
// Every fuzz target defines LLVMFuzzerTestOneInput; linking exactly one of them
// with this file produces a standalone replayer.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

// Provided by the fuzz target this file is linked with.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size);

namespace fs = std::filesystem;

namespace {

// Run one file's bytes through the target. A crash here (ASan/UBSan abort, or a
// target's own __builtin_trap on an invariant violation) is the whole point:
// the process dies with a non-zero status and CI fails on the offending seed.
void replay_file(const fs::path& p, long& count) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "  WARN: cannot open %s\n", p.string().c_str());
        return;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
    ++count;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <corpus-dir-or-file> [more...]\n"
                     "Replays each input through the linked fuzz target.\n",
                     argv[0]);
        return 2;
    }

    long count = 0;
    for (int i = 1; i < argc; ++i) {
        const fs::path arg(argv[i]);
        std::error_code ec;
        if (fs::is_directory(arg, ec)) {
            for (const auto& entry : fs::directory_iterator(arg, ec)) {
                if (entry.is_regular_file(ec)) replay_file(entry.path(), count);
            }
        } else if (fs::is_regular_file(arg, ec)) {
            replay_file(arg, count);
        } else {
            std::fprintf(stderr, "  WARN: skipping %s (not a file or dir)\n", argv[i]);
        }
    }

    // Reaching here means nothing crashed. Print the count so a silently-empty
    // corpus (which would replay nothing and look like a pass) is visible.
    std::printf("replayed %ld input(s) with no crash\n", count);
    return count > 0 ? 0 : 1;
}
