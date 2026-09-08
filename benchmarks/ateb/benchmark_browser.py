"""ATEB Browser Benchmark: Token reduction via semantic AXTree vs vision and raw DOM."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from benchmarks.ateb.benchmark_ast import count_tokens


@dataclass(frozen=True, slots=True)
class BrowserBenchmarkResult:
    """Benchmark metrics comparing Axiom AXTree against vision and raw DOM baselines."""

    scenario: str
    baseline_vision_tokens: int
    baseline_dom_tokens: int
    axiom_axtree_tokens: int
    vision_savings_percent: float
    dom_savings_percent: float


SAMPLE_RAW_HTML_DOM = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Admin Dashboard - AxiomVM Cloud</title>
  <link rel="stylesheet" href="/assets/style.css">
  <script src="/assets/bundle.js" defer></script>
</head>
<body class="bg-gray-100 font-sans leading-normal tracking-normal">
  <header class="bg-white shadow">
    <div class="max-w-7xl mx-auto py-6 px-4 sm:px-6 lg:px-8 flex justify-between items-center">
      <h1 class="text-3xl font-bold leading-tight text-gray-900">Appliance Instances</h1>
      <div class="flex items-center space-x-4">
        <button id="btn-refresh" class="btn btn-secondary">Refresh</button>
        <button id="btn-deploy" class="btn btn-primary bg-blue-600 text-white px-4 py-2 rounded">Deploy New VM</button>
      </div>
    </div>
  </header>
  <main class="max-w-7xl mx-auto py-6 sm:px-6 lg:px-8">
    <div class="px-4 py-6 sm:px-0">
      <div class="border-4 border-dashed border-gray-200 rounded-lg h-96 p-4">
        <table class="min-w-full divide-y divide-gray-200">
          <thead>
            <tr>
              <th class="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider">ID</th>
              <th class="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider">Status</th>
              <th class="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider">VCPU / RAM</th>
              <th class="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase tracking-wider">Actions</th>
            </tr>
          </thead>
          <tbody class="bg-white divide-y divide-gray-200">
            <tr>
              <td class="px-6 py-4 whitespace-nowrap text-sm font-medium text-gray-900">vm-84920</td>
              <td class="px-6 py-4 whitespace-nowrap text-sm text-green-500 font-semibold">Running</td>
              <td class="px-6 py-4 whitespace-nowrap text-sm text-gray-500">2 vCPU / 512 MB</td>
              <td class="px-6 py-4 whitespace-nowrap text-sm font-medium">
                <a href="#view-84920" class="text-indigo-600 hover:text-indigo-900 mr-2">View</a>
                <button data-action="snapshot" class="text-blue-600 hover:text-blue-900 mr-2">Snapshot</button>
                <button data-action="terminate" class="text-red-600 hover:text-red-900">Terminate</button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  </main>
</body>
</html>
"""

SAMPLE_AXTREE = """[1] RootWebArea "Admin Dashboard - AxiomVM Cloud"
  [2] banner
    [3] heading "Appliance Instances" [level: 1]
    [4] button "Refresh"
    [5] button "Deploy New VM"
  [6] main
    [7] table
      [8] row
        [9] columnheader "ID"
        [10] columnheader "Status"
        [11] columnheader "VCPU / RAM"
        [12] columnheader "Actions"
      [13] row
        [14] cell "vm-84920"
        [15] cell "Running"
        [16] cell "2 vCPU / 512 MB"
        [17] cell
          [18] link "View"
          [19] button "Snapshot"
          [20] button "Terminate"
"""

# Standard multimodal vision token count for a single 1280x720 high-res viewport screenshot
VISION_SCREENSHOT_TOKENS = 1600


def run_browser_benchmark(
    token_counter: Callable[[str], int] = count_tokens,
) -> BrowserBenchmarkResult:
    """Executes browser navigation token efficiency benchmark."""
    dom_tokens = token_counter(SAMPLE_RAW_HTML_DOM)
    axtree_tokens = token_counter(SAMPLE_AXTREE)

    vision_savings = (
        (VISION_SCREENSHOT_TOKENS - axtree_tokens) / VISION_SCREENSHOT_TOKENS
    ) * 100.0
    dom_savings = ((dom_tokens - axtree_tokens) / dom_tokens) * 100.0

    return BrowserBenchmarkResult(
        scenario="dashboard_navigation",
        baseline_vision_tokens=VISION_SCREENSHOT_TOKENS,
        baseline_dom_tokens=dom_tokens,
        axiom_axtree_tokens=axtree_tokens,
        vision_savings_percent=vision_savings,
        dom_savings_percent=dom_savings,
    )
