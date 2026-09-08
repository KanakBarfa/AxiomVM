# Agent Guidelines for AxiomVM

## Core Invariants and Rules

- Don't write verbose comments, only one line comments for docstrings.
- Never use em dash and emojis unless asked.
- When making technical decisions, never consider development cost. Prefer quality, maintainability, and scalability.
- Never write yourself as co-author for any commits.
- Run linter and formatter before committing code.
- Don't ignore unrelated bugs, even if they are not in the scope of the current task.
- Don't prioritize basic unit tests, only write integration and end-to-end tests.
- Make use of git to manage code.
- If you see a library is not installed, prompt the user to install it rather than finding a workaround or cheaping out.
- If you see yourself using something that will be helpful as a skill, make a skill out of it or write in project/global AGENTS.md. Skill if its rare conditional requirement, else AGENTS.md.

## AxiomVM Technical Specifications

### 1. C++23 Systems Standard
- Standard: C++23 (`-std=c++23`).
- Invariants: Compiles with `-fno-exceptions` and `-fno-rtti`.
- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror`.
- Error Handling: Use `std::expected<T, SystemError>`. Exceptions are disabled.
- Zero Dynamic Allocation: Once initialized, guest runtime components (`axiom-initd`) must operate with zero dynamic memory allocations (`malloc`, `new`) during regular packet processing and execution loops. Use `axiom::FixedVector<T, N>` or fixed buffers.
- String and Memory Slices: Use `std::string_view` and `std::span<const uint8_t>`.

### 2. Testing Mandate
- Unit tests are deprioritized.
- Only write integration tests and end-to-end tests (such as loopback socketpair/vsock tests, epoll reactor integration, and process isolation tests).
- Integration test binaries are located under `tests/integration/` and executed via `ctest --output-on-failure`.

### 3. Repository Scaffolding and Commands

- Build configuration:
  ```bash
  cmake -B build -G Ninja \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release \
    -DAXIOM_BUILD_TESTS=ON \
    -DAXIOM_WARNINGS_AS_ERRORS=ON
  ```

- Build all targets:
  ```bash
  cmake --build build
  ```

- Run integration tests:
  ```bash
  ctest --test-dir build --output-on-failure
  ```

- Run format check:
  ```bash
  find guest include tests -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) | xargs clang-format --dry-run --Werror
  ```

- Apply code formatting:
  ```bash
  find guest include tests -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -exec clang-format -i {} +
  ```

- Pre-commit checks (using repository virtual environment):
  ```bash
  ./.venv/bin/pre-commit run --all-files
  ```

### 4. Git and Commit Conventions
- Keep commits atomic and focused.
- Run formatters and linters prior to committing changes.
- Never add yourself as co-author.
- Do not push commits to origin unless explicitly instructed by the user.

### 5. Firecracker & Snapshot Operational Invariants
- **CRC64 Validation**: Firecracker validates state snapshots using a reflected MSB-first CRC64 table generated from 8 64-bit basis constants: `(0x7AD870C830358979, 0xF5B0E190606B12F2, 0xC038E5739841B68F, 0xAB28ECB46814FE75, 0x7D08FF3B88BE6F81, 0xFA11FE77117CDF02, 0xDF7ADABD7A6E2D6F, 0x95AC9329AC4BC9B5)`. Any path modifications in state snapshots (`vsock_path`, `rootfs_path`) must recompute the trailing 8-byte CRC64 checksum (`fc_crc64`) masked with `0xFFFFFFFFFFFFFFFF`.
- **Snapshot Load Lifecycle**: `PUT /snapshot/load` can only be invoked on a fresh Firecracker process before boot configuration. MicroVM instance IDs (`--id`) cannot contain underscores (`_`); only alphanumeric characters and hyphens are valid. Pausing and resuming VMs must be executed via `PATCH /vm` with `{"state": "Paused"}` or `{"state": "Resumed"}` (not `PUT /vm/state`).
- **Connection Priming**: Prior to creating a memory snapshot, prime the guest connection with an initial exchange (e.g. `ping()`) so that the guest kernel and PID 1 epoll reactor are fully settled.

### 6. MCP Architecture
- In `mcp >= 2.2.0`, the server is `MCPServer` in `mcp.server.mcpserver` (`server.run(transport='stdio')`).
- Server tool invocations return `CallToolResult` with `content=[TextContent(...)]`.
