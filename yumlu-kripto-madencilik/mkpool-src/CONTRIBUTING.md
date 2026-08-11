# Contributing to mkpool

Thanks for your interest. This repository is the open pool engine, published for
transparency. Contributions that improve correctness, security, performance, or
clarity are welcome.

## Licensing

mkpool is GPLv3. By submitting a contribution you agree it is licensed under the
same GPL-3.0 terms. Keep each source file's license header intact, and add the
same header (SPDX line, GPLv3 notice, copyright) to any new file.

## Building and testing

Full build instructions are in the README. Before opening a pull request:

```bash
# build
cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# unit tests
cd build && ctest --output-on-failure -j

# sanitizers (optional but appreciated)
./scripts/run_sanitizers.sh

# fuzz the stratum parser if you touched protocol handling
HOST=127.0.0.1 PORT=3331 ./scripts/fuzz_suite.sh
```

## Style

- Modern C++23. Match the conventions of the surrounding code.
- Plain ASCII punctuation in code and docs. No fancy dashes or smart quotes.
- Keep each pull request focused on one logical change.

## Pull requests

- Explain what changed and why.
- Call out anything that affects on-chain behavior, share validation, or
  payouts, since those need extra care.
- Make sure the build is clean and the tests pass.

## Reporting issues

For ordinary bugs, open an issue with enough detail to reproduce. For security
problems, follow `SECURITY.md` and report privately rather than opening a public
issue.
