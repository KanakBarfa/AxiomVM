# AxiomVM Architecture

Conventional operating systems are engineered around human perception: display servers, visual compositors, terminal escape codes, and interactive shells. For an AI agent, this interface creates extreme operational waste:

- Burning 1,500+ visual tokens per interaction for screenshot-based mouse coordinate clicks.
- Consuming 2,000+ tokens per command parsing ANSI escape codes, terminal headers, and verbose stdout tables from standard shells.
- Traversing nested file trees via iterative POSIX calls (ls, cd, cat, grep), exhausting context windows before execution begins.

AxiomVM re-engineers the guest operating system runtime from the hypervisor interface up. It strips out human userland (no X11/Wayland, no bash, no systemd) and implements a deterministic, zero-heap semantic appliance written in C++23 running directly on KVM and Firecracker.

## 1. Core Engineering Invariants

Development strictly adheres to a three-tier quality hierarchy:

```
+-------------------------------------------------------------+
| 1. QUALITY (Correctness, Invariant Enforcement, Crash Safety)|
+------------------------------+------------------------------+
                               |
+------------------------------v------------------------------+
| 2. SPEED OPTIMIZATION (Zero-Alloc Steady State, <25ms Boots) |
+------------------------------+------------------------------+
                               |
+------------------------------v------------------------------+
| 3. FEATURE EXPANSION (AXTree CDP, Multi-Language AST)       |
+-------------------------------------------------------------+
```

- **Zero-Heap Steady State**: Once initialized, the guest PID 1 daemon (`axiom-initd`) performs zero dynamic memory allocations (`malloc`, `new`) during regular message routing, signal processing, and frame serialization.
- **Deterministic Failure Modes (`std::expected`)**: The codebase compiles with `-fno-exceptions` and `-fno-rtti`. All fallible system calls and wire decoders return `std::expected<T, SystemError>`. A PID 1 fault that panics the kernel is an unacceptable design defect.
- **Standard-Track Portability**: Built on standard C++23 using GCC 14+ or Clang 18+. Avoids experimental language branches to ensure hermetic static builds with musl-libc and immediate reproducibility across open-source environments.

## 2. System Architecture

```
+---------------------------------------------------------------------------+
|                         HOST ORCHESTRATION LAYER                          |
|          (Python / TypeScript / Rust SDKs via Model Context Protocol)     |
+-------------------------------------+-------------------------------------+
                                      | virtio-vsock (Zero-Network Shared Ring)
+-------------------------------------v-------------------------------------+
|                     HYPERVISOR CORE (Firecracker / KVM)                   |
|   - Sub-25ms MicroVM Cold Boot and Copy-On-Write Memory Snapshots         |
|   - Read-Only Base Image (SquashFS) + Ephemeral Overlay (tmpfs)           |
+-------------------------------------+-------------------------------------+
                                      |
+-------------------------------------v-------------------------------------+
|                     AXIOM GUEST APPLIANCE (PID 1)                         |
|                                                                           |
|  +---------------------------------------------------------------------+  |
|  |                     axiom-initd (C++23 / musl)                      |  |
|  |   - epoll + signalfd + pidfd Native Async Reactor                   |  |
|  |   - Zero-Copy TDS-Wire Protocol (Boost.PFR Reflection)              |  |
|  |   - Fixed-Capacity Stack Structures (FixedVector<T, N>)             |  |
|  +---------------+---------------------+--------------------+----------+  |
|                  |                     |                    |             |
|  +---------------v------+ +------------v---------+ +--------v----------+  |
|  |    AST-VFS Engine    | |   AXTree Web Daemon  | | Filter-at-Source  |  |
|  |  - Tree-sitter C API | |  - Headless Chromium | | Execution Engine  |  |
|  |  - Structural Slices | |  - CDP via Unix Sock | | - PID Isolation   |  |
|  |  - Syntax Diffing    | |  - Element ID Tokens | | - Memory Rings    |  |
|  +----------------------+ +----------------------+ +-------------------+  |
+---------------------------------------------------------------------------+
```

## 3. Architectural Subsystems

### Subsystem A: The Zero-Copy TDS-Wire Protocol
The Token-Dense Serialization Protocol (TDS-Wire) replaces text-based JSON/REST layers with packed binary frames across virtio-vsock:

Frame Topology:
```
[Magic: 2B (0xAA55)][Opcode: 2B][RequestID: 4B][PayloadLen: 4B][Payload Data: NB]
```

Compile-Time Aggregate Reflection via `boost::pfr`:
Uses header-only structured binding reflection to serialize and deserialize POD frames directly without external code generators or runtime overhead:

```cpp
template <typename T>
auto decode_frame(std::span<const uint8_t> buffer) -> std::expected<T, WireError> {
    if (buffer.size() < sizeof(T)) {
        return std::unexpected(WireError::BufferUnderflow);
    }
    T msg{};
    std::memcpy(&msg, buffer.data(), sizeof(T));
    return msg;
}
```

Context-Preserving Pointers: Large system outputs (for example, build logs) remain in the guest scratchpad buffer. The agent receives an indexed summary handle:
```
[HANDLE:0x04A1 | STATUS:FAIL | LINES:840 | ERRORS:1 | SUMMARY:"undefined reference to main"]
```

### Subsystem B: Native Reactor and Process Management
Traditional daemons use complex runtime libraries. `axiom-initd` uses native Linux primitives:

- **Unified Event Reactor**: A single `epoll_create1(EPOLL_CLOEXEC)` loop multiplexes:
  - The `AF_VSOCK` listener file descriptor.
  - `signalfd` for asynchronous, signal-safe child process reaping (`SIGCHLD`) and shutdown hooks.
  - `pidfd` descriptors for monitoring and controlling sandboxed tasks.
- **Filter-at-Source Execution**: Commands execute directly via `clone3()` with unshared mount and IPC namespaces. Process output streams into an inline circular memory buffer:
  - Trailing stdout lines are truncated to relevant diagnostics.
  - ANSI escape sequences are stripped via a single-pass finite-state parser.
  - Returns an exact exit code, CPU cycles consumed, and a structured diff of modified files.

### Subsystem C: AST-Virtual File System (AST-VFS)
Bypasses manual CLI exploration commands (`ls`, `grep`, `cat`):

- **Static Tree-sitter Linking**: Links the pure-C Tree-sitter runtime and specific language grammars (C, C++, Go, Rust) directly into `axiom-initd`.
- **Symbolic Extraction**: The agent invokes `GET_SYMBOL(path, symbol_name)`. The guest parser reads the file, traverses the AST, and extracts only the relevant function or class declaration along with its type signature.
- **Token Reduction**: Reduces context consumption for file discovery and context gathering by 80% to 90%.

### Subsystem D: AXTree Semantic Web Engine
Replaces multi-thousand-token vision screenshots with an accessibility DOM graph:

- **Headless Chromium Integration**: Boots a stripped Chromium headless shell configured to listen on an internal Unix domain socket via the Chrome DevTools Protocol (CDP).
- **Deterministic ID Mapping**: Extracts the accessibility node hierarchy, filters invisible or decorative layout elements, and flattens interactive nodes into an indexed array:
  ```
  [@1] Input (type="search", name="q")
  [@2] Button (text="Search", parent=@1)
  [@3] Link (text="Documentation", target="/docs")
  ```
- **Targeted Dispatch**: The model interacts via lightweight calls (`ACT_CLICK(@2)`, `ACT_TYPE(@1, "Firecracker vsock")`), consuming 30-50 tokens instead of ~1,500 visual tokens.

## 4. C++23 Production Primitives

To enforce the zero-allocation, crash-safe invariant, the codebase replaces traditional heap-heavy STL idioms with verified C++23 patterns:

| Architecture Requirement | Traditional Approach (Avoided) | AxiomVM C++23 Implementation |
|:---|:---|:---|
| Error Handling | C++ Exceptions (`throw`/`catch`) | `std::expected<T, SystemError>` with monadic operations |
| Data Slicing | `std::string`, `std::vector<uint8_t>` | `std::string_view` and `std::span<const uint8_t>` (Zero-Copy) |
| Buffer Allocation | Dynamic Heap (`std::vector`) | `axiom::FixedVector<T, N>` (Stack/BSS-backed array with bounds checking) |
| Process Tracking | Polling `waitpid()` in threads | Non-blocking `epoll` registration of `pidfd` descriptors |
| Reflection / Wire Format | Protobuf / JSON-RPC | Header-only compile-time reflection (`boost::pfr`) over POD structs |

```cpp
template <typename T, size_t Capacity>
class FixedVector {
public:
    constexpr bool push_back(const T& value) noexcept {
        if (size_ >= Capacity) {
            return false;
        }
        data_[size_++] = value;
        return true;
    }

    [[nodiscard]] constexpr size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr std::span<const T> as_span() const noexcept {
        return {data_.data(), size_};
    }

private:
    std::array<T, Capacity> data_{};
    size_t size_{0};
};
```
