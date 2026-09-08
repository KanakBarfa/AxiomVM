# Contributing to AxiomVM

Thank you for your interest in contributing to AxiomVM. AxiomVM is a high-performance, deterministic microVM appliance engineered for AI agents.

## Core Architectural Invariants

All contributions must strictly uphold the following invariants:

1. **Zero Dynamic Allocation in Steady State**:
   - Guest runtime components (`axiom-initd`) must execute zero dynamic memory allocations (`malloc`, `new`) during regular packet processing, signal routing, and execution loops.
   - Use `axiom::FixedVector<T, N>` or static stack/BSS buffers.
2. **Standard-Track C++23**:
   - Compiled with `-std=c++23 -fno-exceptions -fno-rtti`.
   - Compiler warnings enabled as errors: `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`.
   - Error handling via `std::expected<T, SystemError>`.
3. **Firecracker Snapshot Validation**:
   - Firecracker validates state snapshots using a reflected MSB-first CRC64 table generated from 8 64-bit basis constants. Any path modifications (`vsock_path`, `rootfs_path`) must recompute the trailing 8-byte CRC64 checksum masked with `0xFFFFFFFFFFFFFFFF`.
   - VM pause and resume operations use `PATCH /vm` with `{"state": "Paused"}` or `{"state": "Resumed"}`.
4. **Testing Mandate**:
   - Unit tests are deprioritized.
   - Only write integration and end-to-end tests (such as loopback socketpair/vsock tests, epoll reactor integration, and process isolation tests).
   - Test suites execute via `ctest --output-on-failure`.

## Code Style and Conventions

- Run clang-format and pre-commit checks before committing code.
- Write one-line comments for docstrings only; avoid verbose inline commentary.
- Do not use em dashes or emojis in code, commits, or documentation unless explicitly requested.
- Keep commits atomic and focused. Never add yourself as co-author.

## Building and Testing

### Configure and Build
```bash
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DAXIOM_BUILD_TESTS=ON \
  -DAXIOM_WARNINGS_AS_ERRORS=ON
cmake --build build
```

### Run Integration Tests
```bash
ctest --test-dir build --output-on-failure
```

### Pre-commit Verification
```bash
./.venv/bin/pre-commit run --all-files
```
