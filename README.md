# AxiomVM: The Token-First MicroVM Appliance for AI Agents

Conventional operating systems are engineered around human perception: display servers, visual compositors, terminal escape codes, and interactive shells. For an AI agent, this creates extreme operational waste:

- Burning 1,500+ visual tokens per interaction for screenshot-based mouse coordinate clicks.
- Consuming 2,000+ tokens per command parsing ANSI escape codes, terminal headers, and verbose stdout tables from standard shells.
- Traversing nested file trees via iterative POSIX calls (`ls`, `cd`, `cat`, `grep`), exhausting context windows before execution begins.

AxiomVM re-engineers the guest operating system runtime from the hypervisor interface up. It strips out human userland (no X11/Wayland, no bash, no systemd) and implements a deterministic, zero-heap semantic appliance written in C++23 running directly on KVM and Firecracker.

---

## Performance & Token Savings (ATEB Benchmark)

The Axiom Token Efficiency Benchmark (ATEB) suite quantifies real token reductions across execution, code discovery, and browser navigation:

| Scenario | Baseline Modality | AxiomVM Appliance | Token Reduction | Latency |
|:---|:---|:---|:---|:---|
| **AST Discovery** | POSIX `cat` (261 tokens) | `axiom_ast_slice` (98 tokens) | **62.5% reduction** | < 1 ms |
| **Exec Filtering** | Raw ANSI shell (215 tokens) | Bounded memory ring + ANSI strip (117 tokens) | **45.6% reduction** | < 1 ms |
| **Exec Diffs** | Full file `cat` verification (92 tokens) | Single-pass unified diff (32 tokens) | **65.2% reduction** | < 2 ms |
| **Browser Interaction** | Multimodal Vision 720p (1,600 tokens) | Semantic AXTree handles (141 tokens) | **91.2% reduction** | < 10 ms |
| **Browser DOM** | Raw HTML DOM (615 tokens) | Semantic AXTree handles (141 tokens) | **77.1% reduction** | < 10 ms |
| **Sandbox Branching** | Cold VM boot (157.6 ms) | Copy-on-write memory restore (3.74 ms) | **42.1x speedup** | **3.74 ms** |

*For full benchmark methodologies and reproduction steps, see [benchmarks/ateb/REPORT.md](file:///home/kanak/AxiomVM/benchmarks/ateb/REPORT.md).*

---

## Quickstart Installation

You can set up and run AxiomVM on any Linux host (x86_64) in under two minutes using prebuilt release assets.

### 1. Prerequisites

AxiomVM requires Linux with KVM hardware virtualization enabled:

```bash
# Enable KVM permissions for current user
sudo usermod -aG kvm $USER
newgrp kvm
```

Install Firecracker (v1.10+):
```bash
ARCH="$(uname -m)"
curl -L "https://github.com/firecracker-microvm/firecracker/releases/download/v1.10.1/firecracker-v1.10.1-${ARCH}.tgz" | tar -xz
sudo mv "release-v1.10.1-${ARCH}/firecracker-v1.10.1-${ARCH}" /usr/local/bin/firecracker
sudo chmod +x /usr/local/bin/firecracker
```

### 2. Download and Install

Install the Python CLI orchestrator and download prebuilt appliance assets:

```bash
# Install AxiomVM Python package
pip install https://github.com/KanakBarfa/AxiomVM/releases/download/v0.1.0/axiom_vm-0.1.0-py3-none-any.whl

# Download appliance assets
mkdir -p ~/.axiom && cd ~/.axiom
curl -LO https://github.com/KanakBarfa/AxiomVM/releases/download/v0.1.0/vmlinux
curl -LO https://github.com/KanakBarfa/AxiomVM/releases/download/v0.1.0/axiom-rootfs.ext4

# Set asset environment variables (or add to ~/.bashrc)
export AXIOM_KERNEL_PATH="$HOME/.axiom/vmlinux"
export AXIOM_ROOTFS_PATH="$HOME/.axiom/axiom-rootfs.ext4"
```

### 3. Verify and Run

Check host prerequisites:
```bash
axiom check
```

Execute a command inside an isolated microVM:
```bash
axiom exec "uname -a"
```

Boot an interactive appliance instance:
```bash
axiom run
```

---

## AI Agent Integration (Model Context Protocol / MCP)

AxiomVM includes a native Model Context Protocol (MCP 2.x) daemon in [`host/axiom/mcp_server.py`](file:///home/kanak/AxiomVM/host/axiom/mcp_server.py) providing 8 tools for AI coding assistants:

| MCP Tool | Description |
|:---|:---|
| `axiom_exec` | Executes commands inside the microVM with ANSI stripping and structured file diffs. |
| `axiom_ast_symbols` | Returns concise symbol declarations from C, C++, Go, or Rust source files. |
| `axiom_ast_slice` | Extracts the exact definition body of a function, struct, or class. |
| `axiom_ast_patch` | Replaces a symbol definition atomically and returns updated line maps. |
| `axiom_browser_navigate` | Navigates headless Chromium and returns an accessible semantic node tree (`[@1] button "Submit"`). |
| `axiom_browser_action` | Dispatches click, type, or scroll actions against semantic element IDs without screenshots. |
| `axiom_snapshot` | Creates a copy-on-write memory checkpoint of the running microVM in ~3ms. |
| `axiom_branch` | Restores or branches from a named snapshot state instantly. |

### Configuration for Claude Desktop, Cursor, Antigravity, and OpenCode

#### Antigravity (`~/.gemini/config/mcp_config.json`)
```json
{
  "mcpServers": {
    "axiom": {
      "command": "axiom-mcp",
      "env": {
        "AXIOM_KERNEL_PATH": "/home/user/.axiom/vmlinux",
        "AXIOM_ROOTFS_PATH": "/home/user/.axiom/axiom-rootfs.ext4"
      }
    }
  }
}
```

#### OpenCode (`~/.config/opencode/opencode.jsonc`)
```jsonc
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "axiom": {
      "type": "local",
      "command": ["axiom-mcp"],
      "environment": {
        "AXIOM_KERNEL_PATH": "/home/user/.axiom/vmlinux",
        "AXIOM_ROOTFS_PATH": "/home/user/.axiom/axiom-rootfs.ext4"
      }
    }
  }
}
```

#### Claude Desktop (`claude_desktop_config.json`) / Cursor (`cursor.json`)
```json
{
  "mcpServers": {
    "axiom": {
      "command": "axiom-mcp",
      "env": {
        "AXIOM_KERNEL_PATH": "/home/user/.axiom/vmlinux",
        "AXIOM_ROOTFS_PATH": "/home/user/.axiom/axiom-rootfs.ext4"
      }
    }
  }
}
```

---

## TypeScript SDK (`@axiom/sdk`)

For Node.js and TypeScript agent frameworks, install the client package from the release:

```bash
npm install https://github.com/KanakBarfa/AxiomVM/releases/download/v0.1.0/axiom-sdk-0.1.0.tgz
```

```typescript
import { AxiomClient } from "@axiom/sdk";

const client = new AxiomClient({ vsockPort: 5200 });
await client.connect();

const result = await client.exec("ls -la /tmp");
console.log("Exit code:", result.exitCode);
console.log("Output:", result.output);

await client.disconnect();
```

---

## Architecture and Design Invariants

AxiomVM enforces strict low-level invariants:
- **Zero Dynamic Heap Allocations**: No `malloc` or `new` during steady-state request loops in guest PID 1.
- **Fail-Safe Determinism**: Compiled with `-fno-exceptions` and `-fno-rtti`; all fallible operations return `std::expected<T, SystemError>`.
- **TDS-Wire Binary Protocol**: Direct 12-byte packed binary framing across virtio-vsock shared memory rings.
- **Reflected MSB-First CRC64 Snapshots**: Deterministic snapshot memory and device tree rewrites.

*For detailed architectural subsystem specifications, memory layouts, and wire protocols, see [ARCHITECTURE.md](file:///home/kanak/AxiomVM/ARCHITECTURE.md).*

---

## Building from Source

To build AxiomVM from source and run the full integration test suite:

```bash
# Clone repository
git clone https://github.com/KanakBarfa/AxiomVM.git
cd AxiomVM

# Configure build with Ninja
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DAXIOM_BUILD_TESTS=ON \
  -DAXIOM_WARNINGS_AS_ERRORS=ON

# Compile targets
cmake --build build

# Run integration test suite
ctest --test-dir build --output-on-failure
```

*For contribution guidelines, code formatting standards, and hermetic musl container build instructions, see [CONTRIBUTING.md](file:///home/kanak/AxiomVM/CONTRIBUTING.md).*

---

## License

AxiomVM is distributed under the [Apache License 2.0](file:///home/kanak/AxiomVM/LICENSE).
