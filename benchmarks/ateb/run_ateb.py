"""ATEB (Axiom Token Efficiency Benchmark) unified test harness and report generator."""

from __future__ import annotations

import json
from pathlib import Path

from benchmarks.ateb.benchmark_ast import run_ast_benchmark
from benchmarks.ateb.benchmark_browser import run_browser_benchmark
from benchmarks.ateb.benchmark_exec import run_exec_benchmark
from benchmarks.ateb.benchmark_snapshot import run_snapshot_benchmark


def main() -> None:
    """Executes all ATEB benchmarks and emits structured JSON and Markdown reports."""
    print("Running Axiom Token Efficiency Benchmark (ATEB) suite...")

    ast_res = run_ast_benchmark()
    exec_res_list = run_exec_benchmark()
    browser_res = run_browser_benchmark()
    snapshot_res = run_snapshot_benchmark(iterations=2)

    report_data = {
        "ast_benchmark": {
            "file": ast_res.file_name,
            "raw_file_tokens": ast_res.raw_file_tokens,
            "symbols_outline_tokens": ast_res.symbols_outline_tokens,
            "slice_tokens": ast_res.slice_tokens,
            "axiom_total_tokens": ast_res.axiom_total_tokens,
            "tokens_saved": ast_res.tokens_saved,
            "reduction_percent": round(ast_res.reduction_percent, 2),
        },
        "exec_benchmarks": [
            {
                "scenario": item.scenario,
                "raw_shell_tokens": item.raw_shell_tokens,
                "axiom_exec_tokens": item.axiom_exec_tokens,
                "tokens_saved": item.tokens_saved,
                "reduction_percent": round(item.reduction_percent, 2),
            }
            for item in exec_res_list
        ],
        "browser_benchmark": {
            "scenario": browser_res.scenario,
            "baseline_vision_tokens": browser_res.baseline_vision_tokens,
            "baseline_dom_tokens": browser_res.baseline_dom_tokens,
            "axiom_axtree_tokens": browser_res.axiom_axtree_tokens,
            "vision_savings_percent": round(browser_res.vision_savings_percent, 2),
            "dom_savings_percent": round(browser_res.dom_savings_percent, 2),
        },
        "snapshot_benchmark": (
            {
                "iterations": snapshot_res.iterations,
                "cold_boot_mean_ms": round(snapshot_res.cold_boot_mean_ms, 2),
                "snapshot_restore_mean_ms": round(
                    snapshot_res.snapshot_restore_mean_ms, 2
                ),
                "branch_spawn_mean_ms": round(snapshot_res.branch_spawn_mean_ms, 2),
                "speedup_factor": round(snapshot_res.speedup_factor, 2),
            }
            if snapshot_res
            else "KVM/Artifacts unavailable"
        ),
    }

    out_dir = Path(__file__).resolve().parent
    json_path = out_dir / "ateb_report.json"
    json_path.write_text(json.dumps(report_data, indent=2), encoding="utf-8")

    md_lines = [
        "# Axiom Token Efficiency Benchmark (ATEB) Results",
        "",
        "## 1. AST Structural Slicing",
        "| Metric | Baseline (POSIX cat) | Axiom (Outline + Slice) | Reduction |",
        "|---|---|---|---|",
        f"| Tokens | {ast_res.raw_file_tokens} | {ast_res.axiom_total_tokens} | **{ast_res.reduction_percent:.1f}%** |",
        "",
        "## 2. Command Execution & Diff Capture",
        "| Scenario | Raw Shell Tokens | Axiom Tokens | Reduction |",
        "|---|---|---|---|",
    ]
    for ex in exec_res_list:
        md_lines.append(
            f"| {ex.scenario} | {ex.raw_shell_tokens} | {ex.axiom_exec_tokens} | **{ex.reduction_percent:.1f}%** |"
        )

    md_lines.extend(
        [
            "",
            "## 3. Browser Automation",
            "| Baseline Modality | Baseline Tokens | Axiom AXTree Tokens | Reduction |",
            "|---|---|---|---|",
            f"| Vision (1280x720 screenshot) | {browser_res.baseline_vision_tokens} | {browser_res.axiom_axtree_tokens} | **{browser_res.vision_savings_percent:.1f}%** |",
            f"| Raw HTML DOM | {browser_res.baseline_dom_tokens} | {browser_res.axiom_axtree_tokens} | **{browser_res.dom_savings_percent:.1f}%** |",
            "",
        ]
    )

    if snapshot_res:
        md_lines.extend(
            [
                "## 4. Snapshot & Branching Latency",
                "| Operation | Latency (ms) | Target |",
                "|---|---|---|",
                f"| Cold Boot | {snapshot_res.cold_boot_mean_ms:.2f} ms | < 25.0 ms |",
                f"| Snapshot Restore | {snapshot_res.snapshot_restore_mean_ms:.2f} ms | < 5.0 ms |",
                f"| Branch Spawn | {snapshot_res.branch_spawn_mean_ms:.2f} ms | < 25.0 ms |",
                "",
                f"Snapshot restore yields a **{snapshot_res.speedup_factor:.1f}x speedup** over cold boot.",
            ]
        )

    md_path = out_dir / "REPORT.md"
    md_path.write_text("\n".join(md_lines) + "\n", encoding="utf-8")
    print(f"ATEB benchmark complete. Reports written to {json_path} and {md_path}")


if __name__ == "__main__":
    main()
