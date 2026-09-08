# Axiom Token Efficiency Benchmark (ATEB) Results

## 1. AST Structural Slicing
| Metric | Baseline (POSIX cat) | Axiom (Outline + Slice) | Reduction |
|---|---|---|---|
| Tokens | 261 | 98 | **62.5%** |

## 2. Command Execution & Diff Capture
| Scenario | Raw Shell Tokens | Axiom Tokens | Reduction |
|---|---|---|---|
| ansi_test_output | 215 | 117 | **45.6%** |
| diff_vs_cat_feedback | 92 | 32 | **65.2%** |

## 3. Browser Automation
| Baseline Modality | Baseline Tokens | Axiom AXTree Tokens | Reduction |
|---|---|---|---|
| Vision (1280x720 screenshot) | 1600 | 141 | **91.2%** |
| Raw HTML DOM | 615 | 141 | **77.1%** |

## 4. Snapshot & Branching Latency
| Operation | Latency (ms) | Target |
|---|---|---|
| Cold Boot | 157.60 ms | < 25.0 ms |
| Snapshot Restore | 3.74 ms | < 5.0 ms |
| Branch Spawn | 4.70 ms | < 25.0 ms |

Snapshot restore yields a **42.1x speedup** over cold boot.
