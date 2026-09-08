"""ATEB Exec Benchmark: Execution token reduction via ANSI stripping and diff capture."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from benchmarks.ateb.benchmark_ast import count_tokens


@dataclass(frozen=True, slots=True)
class ExecBenchmarkResult:
    """Benchmark metrics comparing Axiom exec response against raw shell baseline."""

    scenario: str
    raw_shell_tokens: int
    axiom_exec_tokens: int
    tokens_saved: int
    reduction_percent: float


RAW_NPM_TEST_OUTPUT = """\x1b[33m\x1b[1m> test-runner@1.0.0 test\x1b[22m\x1b[39m
\x1b[33m> jest --verbose\x1b[39m

\x1b[1m\x1b[32m PASS \x1b[39m\x1b[22m \x1b[2mtests/\x1b[22m\x1b[1mauth.test.ts\x1b[22m
  \x1b[1mAuthentication Module\x1b[22m
    \x1b[32m✓\x1b[39m \x1b[2mshould issue valid JWT with standard claims\x1b[22m\x1b[2m (42 ms)\x1b[22m
    \x1b[32m✓\x1b[39m \x1b[2mshould reject expired token signature\x1b[22m\x1b[2m (15 ms)\x1b[22m
    \x1b[32m✓\x1b[39m \x1b[2mshould rotate signing keys seamlessly\x1b[22m\x1b[2m (19 ms)\x1b[22m

\x1b[1m\x1b[32m PASS \x1b[39m\x1b[22m \x1b[2mtests/\x1b[22m\x1b[1mrouter.test.ts\x1b[22m
  \x1b[1mRoute Dispatcher\x1b[22m
    \x1b[32m✓\x1b[39m \x1b[2mroutes GET /api/v1/health\x1b[22m\x1b[2m (8 ms)\x1b[22m
    \x1b[32m✓\x1b[39m \x1b[2mdispatches request to worker pool\x1b[22m\x1b[2m (23 ms)\x1b[22m

\x1b[1mTest Suites:\x1b[22m \x1b[1m\x1b[32m2 passed\x1b[39m\x1b[22m, 2 total
\x1b[1mTests:\x1b[22m       \x1b[1m\x1b[32m5 passed\x1b[39m\x1b[22m, 5 total
\x1b[1mSnapshots:\x1b[22m   0 total
\x1b[1mTime:\x1b[22m        1.842 s
\x1b[2mRan all test suites.\x1b[22m
"""

AXIOM_STRIPPED_OUTPUT = """PASS tests/auth.test.ts
  Authentication Module
    ✓ should issue valid JWT with standard claims (42 ms)
    ✓ should reject expired token signature (15 ms)
    ✓ should rotate signing keys seamlessly (19 ms)

PASS tests/router.test.ts
  Route Dispatcher
    ✓ routes GET /api/v1/health (8 ms)
    ✓ dispatches request to worker pool (23 ms)

Test Suites: 2 passed, 2 total
Tests:       5 passed, 5 total
Snapshots:   0 total
Time:        1.842 s
Ran all test suites.
"""

RAW_FILE_EDIT_AND_CAT = """$ sed -i 's/max_concurrency = 4/max_concurrency = 16/' config.ini
$ cat config.ini
[server]
listen_address = 0.0.0.0
port = 8080
max_concurrency = 16
enable_logging = true
timeout_seconds = 30
keepalive_timeout = 60
ssl_certificate = /etc/ssl/certs/axiom.pem
ssl_private_key = /etc/ssl/private/axiom.key
worker_threads = 8
io_ring_entries = 1024
max_connections = 65536
"""

AXIOM_EDIT_DIFF = """--- a/config.ini
+++ b/config.ini
@@ -3,3 +3,3 @@
 port = 8080
-max_concurrency = 4
+max_concurrency = 16
 enable_logging = true
"""


def run_exec_benchmark(
    token_counter: Callable[[str], int] = count_tokens,
) -> list[ExecBenchmarkResult]:
    """Executes execution token efficiency benchmarks."""
    results: list[ExecBenchmarkResult] = []

    # Scenario 1: Test output ANSI stripping
    raw_ansi_tokens = token_counter(RAW_NPM_TEST_OUTPUT)
    axiom_clean_tokens = token_counter(AXIOM_STRIPPED_OUTPUT)
    saved_ansi = raw_ansi_tokens - axiom_clean_tokens
    red_ansi = (saved_ansi / raw_ansi_tokens) * 100.0 if raw_ansi_tokens > 0 else 0.0

    results.append(
        ExecBenchmarkResult(
            scenario="ansi_test_output",
            raw_shell_tokens=raw_ansi_tokens,
            axiom_exec_tokens=axiom_clean_tokens,
            tokens_saved=saved_ansi,
            reduction_percent=red_ansi,
        )
    )

    # Scenario 2: File edit feedback (git diff vs full file cat)
    raw_cat_tokens = token_counter(RAW_FILE_EDIT_AND_CAT)
    diff_tokens = token_counter(AXIOM_EDIT_DIFF)
    saved_diff = raw_cat_tokens - diff_tokens
    red_diff = (saved_diff / raw_cat_tokens) * 100.0 if raw_cat_tokens > 0 else 0.0

    results.append(
        ExecBenchmarkResult(
            scenario="diff_vs_cat_feedback",
            raw_shell_tokens=raw_cat_tokens,
            axiom_exec_tokens=diff_tokens,
            tokens_saved=saved_diff,
            reduction_percent=red_diff,
        )
    )

    return results
