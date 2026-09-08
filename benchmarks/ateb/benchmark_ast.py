"""ATEB AST Benchmark: SWE-bench style token reduction via structural slicing."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

try:
    import tiktoken  # type: ignore[import-not-found]

    _enc = tiktoken.get_encoding("cl100k_base")

    def count_tokens(text: str) -> int:
        """Counts exact BPE tokens using cl100k_base encoding."""
        return len(_enc.encode(text))
except ImportError:

    def count_tokens(text: str) -> int:
        """Estimates token count when tiktoken is not available."""
        return max(1, len(text) // 4)


@dataclass(frozen=True, slots=True)
class AstBenchmarkResult:
    """Benchmark metrics comparing AST slicing against baseline POSIX cat."""

    file_name: str
    raw_file_tokens: int
    symbols_outline_tokens: int
    slice_tokens: int
    axiom_total_tokens: int
    tokens_saved: int
    reduction_percent: float


SAMPLE_CPP_FILE = """#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <system_error>

namespace axiom {

struct ServerConfig {
    std::string listen_address;
    uint16_t port;
    size_t max_concurrency;
    bool enable_logging;
};

class ConnectionPool {
public:
    explicit ConnectionPool(size_t capacity);
    ~ConnectionPool();

    bool acquire();
    void release();
    size_t active_count() const;
    void reset_all();

private:
    size_t capacity_;
    size_t active_;
};

int compute_hash(const std::string& key) {
    int h = 0;
    for (char c : key) {
        h = (h * 31) + static_cast<int>(c);
    }
    return h;
}

int target_function_to_fix(int x, int y) {
    // Intentional off-by-one bug here
    if (x <= 0) {
        return y;
    }
    int acc = 0;
    for (int i = 0; i < x; ++i) {
        acc += (y * i);
    }
    return acc;
}

class RequestRouter {
public:
    RequestRouter();
    void register_route(const std::string& route);
    void dispatch(const std::string& path);
};

} // namespace axiom
"""

SAMPLE_SYMBOLS_OUTLINE = """[struct] ServerConfig [L9-L14]
[class] ConnectionPool [L16-L28]
[function] compute_hash [L30-L36]
[function] target_function_to_fix [L38-L47]
[class] RequestRouter [L49-L53]"""

SAMPLE_TARGET_SLICE = """int target_function_to_fix(int x, int y) {
    // Intentional off-by-one bug here
    if (x <= 0) {
        return y;
    }
    int acc = 0;
    for (int i = 0; i < x; ++i) {
        acc += (y * i);
    }
    return acc;
}"""


def run_ast_benchmark(
    token_counter: Callable[[str], int] = count_tokens,
) -> AstBenchmarkResult:
    """Executes AST token efficiency benchmark against POSIX cat baseline."""
    raw_tokens = token_counter(SAMPLE_CPP_FILE)
    symbols_tokens = token_counter(SAMPLE_SYMBOLS_OUTLINE)
    slice_tokens = token_counter(SAMPLE_TARGET_SLICE)
    axiom_total = symbols_tokens + slice_tokens

    saved = raw_tokens - axiom_total
    reduction = (saved / raw_tokens) * 100.0 if raw_tokens > 0 else 0.0

    return AstBenchmarkResult(
        file_name="server_router.cpp",
        raw_file_tokens=raw_tokens,
        symbols_outline_tokens=symbols_tokens,
        slice_tokens=slice_tokens,
        axiom_total_tokens=axiom_total,
        tokens_saved=saved,
        reduction_percent=reduction,
    )
