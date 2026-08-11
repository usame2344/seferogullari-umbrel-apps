# Wire-surface fuzzing

Fuzz targets for everything an unauthenticated peer can put on the wire, across
both Stratum V1 and V2.

## Why these exist

On **2026-07-15** a peer sent an SV2 frame header declaring `msg_length=200000`.
The framing code trusted it, computed `msg_length + 16` outstanding bytes, and
handed that straight to `asio::buffer()` over a fixed 16 KiB `read_buf_`. The
result was a ~183 KiB heap overflow that corrupted adjacent mutex objects and
crash-looped four mainnet pools until the source IPs were blocked.

The bug was **not** in the parser. `sv2::Reader` bounds-checks every read and
throws. The bug was in what a caller *did* with a length the parser had happily
returned. That distinction drives the design here:

| Target | Protocol | Covers | Would it have caught 2026-07-15? |
|---|---|---|---|
| `fuzz_sv2_frame` | V2 | header parse → length gate → read sizing | **Yes.** This is the exact bug class. |
| `fuzz_sv2_message` | V2 | all 9 client→server message parsers | No. The parser was never at fault. |
| `fuzz_sv2_codec` | V2 | `Reader` primitives in fuzzer-chosen order | No, but it covers the length-prefixed reads. |
| `fuzz_address` | V1 | payout-address decoding (`mining.authorize`) | n/a, different surface, same lesson. |
| `fuzz_stratum_configure` | V1 | `mining.configure` negotiation | n/a, different surface, same lesson. |

## The `noexcept` trap (V1)

Both V1 targets exist for a specific reason. `address::decode()` and
`stratum::negotiate_configure()` are **`noexcept` and fed attacker-controlled
data**. For a noexcept function an escaping exception is not an error return, it
is `std::terminate`: the whole pool process dies and every miner drops. Every
type check inside `negotiate_configure` (and there are many) is load-bearing:
one `.get<std::string>()` on a peer-supplied non-string is a remote kill switch.
These targets are what prove those checks hold.

`fuzz_sv2_frame` asserts the invariant that was violated: **no peer-supplied
number may ever cause us to request more bytes than the destination buffer can
hold.** Point it at the pre-patch code and it aborts on the first oversized
length.

## Two ways to run: CI replay (g++) and local exploration (clang)

Each target defines `LLVMFuzzerTestOneInput`. That single entry point is driven
two ways.

### CI: corpus replay under g++ (no clang)

CI does not do coverage-guided fuzzing. Coverage-guided fuzzing needs clang's
libFuzzer, and clang's libstdc++ autodetection on the CI base image cannot be
pinned reliably. So CI **replays** the committed corpus (which includes the
literal 2026-07-15 attack payload) through each target under ASan/UBSan, linked
against `replay_main.cpp` instead of libFuzzer, using the same g++ the rest of
the build already passes with. A reintroduced bug crashes on a seed; the gate
stays green otherwise. This is what `.github/workflows/fuzz.yml` runs.

Reproduce a CI replay locally:

```bash
# Standalone SV2 target (header-only):
g++ -std=c++23 -I src -fsanitize=address,undefined -g -O1 \
  -Wl,--version-script=cmake/hidden-exports.map \
  tests/fuzz/fuzz_sv2_frame.cpp tests/fuzz/replay_main.cpp -o fuzz_sv2_frame
ASAN_OPTIONS=detect_leaks=0 ./fuzz_sv2_frame tests/fuzz/corpus

# V1 target (compiles a mkpool source in; needs system fmt/spdlog/nlohmann):
g++ -std=c++23 -I src $(pkg-config --cflags libpqxx libzmq libsodium libsecp256k1) \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY -fsanitize=address,undefined -g -O1 \
  -Wl,--version-script=cmake/hidden-exports.map \
  tests/fuzz/fuzz_address.cpp src/address.cpp tests/fuzz/replay_main.cpp \
  -lssl -lcrypto -lpqxx -lpq -o fuzz_address
ASAN_OPTIONS=detect_leaks=0 ./fuzz_address tests/fuzz/corpus-v1
```

The `--version-script` is not optional for the V1 targets: they instantiate
`pqxx::type_name<T>`, which under GCC 15 exports from the executable, preempts
libpqxx.so, and double-frees at exit (see `cmake/hidden-exports.map`).
`detect_leaks=0` because replay hunts memory *corruption*, not leaks.

### Local: real coverage-guided fuzzing under clang

To actually explore (mutate inputs to find NEW paths), build a target with clang
and libFuzzer. clang must be told which libstdc++ to use, because it otherwise
picks a header-less newer GCC install dir:

```bash
V=$(g++ -dumpversion); TRIPLE=$(g++ -dumpmachine)
STDCXX="-nostdinc++ -isystem /usr/include/c++/$V -isystem /usr/include/$TRIPLE/c++/$V -isystem /usr/include/c++/$V/backward"

clang++ $STDCXX -std=c++23 -I src \
  -fsanitize=fuzzer,address,undefined -g -O1 \
  tests/fuzz/fuzz_sv2_frame.cpp -o fuzz_sv2_frame
./fuzz_sv2_frame tests/fuzz/corpus -dict=tests/fuzz/sv2.dict -max_total_time=300
```

For a V1 target under clang, add the same source, `pkg-config` cflags, libs, and
`--version-script` as the g++ command above.

Always run with sanitizers on. Without ASan a heap overflow is silent corruption
that surfaces later as an unrelated crash, which is precisely why the original
bug presented as three unrelated symptoms (`double free or corruption`, SIGSEGV
in `pthread_mutex_lock`, and a glibc `tpp.c` assert) rather than one obvious one.

```bash
clang++ -std=c++23 -I src -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -fsanitize=fuzzer,address,undefined -g -O1 \
  tests/fuzz/fuzz_address.cpp src/address.cpp \
  -lssl -lcrypto -lpqxx -lpq -o fuzz_address

./fuzz_address tests/fuzz/corpus-v1 -max_total_time=300
```

## Corpus

`corpus/` (V2) and `corpus-v1/` (V1) hold regression seeds, kept in git on
purpose:

- `frame_attack_msglen_200000`: the literal payload from the incident
  (`00 00 00 40 0d 03`: ext=0, type=0x00, msg_len=200000). If the length gate
  ever regresses, this is the first input replayed.
- `frame_len_max_65519` / `frame_len_max_plus1_65520`: the exact off-by-one
  boundary either side of `MAX_MSG_LENGTH`.
- `frame_len_u24_max`: the largest length the 24-bit wire field can express.
- `codec_*_overclaim`: length prefixes claiming more bytes than are present.
- `corpus-v1/addr_*`: one real address per encoding (bech32, bech32m, base58
  P2PKH/P2SH, cashaddr, uppercase). Mutating a *valid* address is far more
  productive than random bytes, which never get past the checksum.
- `corpus-v1/cfg_mask_overreach`: a miner demanding `version-rolling.mask =
  ffffffff`. The invariant is that we still grant only our own mask.

`sv2.dict` gives libFuzzer the structural tokens (valid message types, length
encodings around the bound) so it spends its budget on logic rather than on
rediscovering the wire format.

## Deterministic counterparts

Two files pin the same properties with fixed values and run in the normal Catch2
suite on every build, no clang required:

- `tests/test_sv2_framing.cpp`: the V2 length bound and the read-sizing invariant.
- `tests/test_stratum_v1_hostile.cpp`: V1 adversarial input: malformed
  `mining.configure` params, hostile payout addresses, bad submit hex.

The fuzzer explores; those tests stop specific known-bad values from ever coming
back. They also pin two behaviours that look like bugs and are not:

- **Uppercase bech32 is valid** (BIP173: all-lower or all-upper; mixed case is
  invalid). "Reject anything that looks weird" would silently break payouts for
  every miner whose wallet emits uppercase.
- **`valid_hex()` does not imply `hex_to_bytes()` succeeds**: `valid_hex` checks
  the alphabet, not that the length is even. Any new caller must check both.

## Adding a message

If a new client→server message gains a `deserialize()`, add it to the switch in
`fuzz_sv2_message.cpp`. Anything not in that switch ships unfuzzed.
