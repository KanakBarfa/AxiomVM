#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "axiom/common.hpp"
#include "axiom/diff.hpp"
#include "axiom/process.hpp"
#include "axiom/wire.hpp"

/// Asserts a condition or prints diagnostics and terminates.
static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "[FAIL] %s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

/// Tests basic command execution and standard output capture.
static void test_basic_exec() {
    axiom::exec::ProcessRunner runner;
    std::array<char, 4096> out_buf{};
    std::array<char, 4096> diff_buf{};

    axiom::exec::ExecOptions opts{
        .command = "echo 'Hello AxiomVM Process Runner'",
        .working_dir = "/tmp",
        .timeout_ms = 5000,
        .flags = axiom::wire::EXEC_FLAG_STRIP_ANSI,
    };

    auto res = runner.run(opts, out_buf, diff_buf);
    check(res.has_value(), "ProcessRunner::run failed");
    check(res->exit_code == 0, "Expected exit code 0");
    check(res->cpu_cycles > 0, "Expected CPU cycles > 0");

    std::string_view output(out_buf.data(), res->output_len);
    check(output == "Hello AxiomVM Process Runner\n", "Output mismatch");
    std::printf("[PASS] test_basic_exec passed (cycles: %lu)\n",
                static_cast<unsigned long>(res->cpu_cycles));
}

/// Tests single-pass ANSI escape code stripping on process output.
static void test_ansi_stripping() {
    axiom::exec::ProcessRunner runner;
    std::array<char, 4096> out_buf{};
    std::array<char, 4096> diff_buf{};

    axiom::exec::ExecOptions opts{
        .command = "printf '\\033[31;1mError:\\033[0m \\033[32mSuccess\\033[0m\\n'",
        .working_dir = "/tmp",
        .timeout_ms = 5000,
        .flags = axiom::wire::EXEC_FLAG_STRIP_ANSI,
    };

    auto res = runner.run(opts, out_buf, diff_buf);
    check(res.has_value(), "ProcessRunner::run failed");
    check(res->exit_code == 0, "Expected exit code 0");

    std::string_view output(out_buf.data(), res->output_len);
    check(output == "Error: Success\n", "Stripped output mismatch");
    std::printf("[PASS] test_ansi_stripping passed\n");
}

/// Tests file delta extraction across command execution boundary.
static void test_file_diff_extraction() {
    char test_dir[] = "/tmp/axiom_diff_test_XXXXXX";
    char* created_dir = mkdtemp(test_dir);
    check(created_dir != nullptr, "mkdtemp failed");

    char initial_file[128];
    std::snprintf(initial_file, sizeof(initial_file), "%s/initial.txt", created_dir);
    FILE* f_init = std::fopen(initial_file, "w");
    check(f_init != nullptr, "fopen initial_file failed");
    std::fputs("v1", f_init);
    std::fclose(f_init);

    axiom::exec::ProcessRunner runner;
    std::array<char, 4096> out_buf{};
    std::array<char, 4096> diff_buf{};

    char cmd[512];
    std::snprintf(cmd, sizeof(cmd),
                  "echo 'new file content' > %s/created.txt && "
                  "echo 'longer v2 content' > %s/initial.txt",
                  created_dir, created_dir);

    axiom::exec::ExecOptions opts{
        .command = cmd,
        .working_dir = created_dir,
        .timeout_ms = 5000,
        .flags = axiom::wire::EXEC_FLAG_CAPTURE_DIFF,
    };

    auto res = runner.run(opts, out_buf, diff_buf);
    check(res.has_value(), "ProcessRunner::run failed");
    check(res->exit_code == 0, "Expected exit code 0");
    check(res->diff_len > 0, "Expected non-empty diff");

    std::string_view diff_str(diff_buf.data(), res->diff_len);
    check(diff_str.find("+ ") != std::string_view::npos, "Diff missing '+'");
    check(diff_str.find("created.txt") != std::string_view::npos, "Diff missing created.txt");
    check(diff_str.find("M ") != std::string_view::npos, "Diff missing 'M'");
    check(diff_str.find("initial.txt") != std::string_view::npos, "Diff missing initial.txt");

    // Clean up
    char rm_cmd[256];
    std::snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", created_dir);
    if (system(rm_cmd) != 0) {
        // Ignore cleanup error
    }

    std::printf("[PASS] test_file_diff_extraction passed\n");
}

/// Tests timeout detection and SIGKILL termination via pidfd.
static void test_timeout_enforcement() {
    axiom::exec::ProcessRunner runner;
    std::array<char, 4096> out_buf{};
    std::array<char, 4096> diff_buf{};

    axiom::exec::ExecOptions opts{
        .command = "sleep 10",
        .working_dir = "/tmp",
        .timeout_ms = 150,
        .flags = 0,
    };

    auto res = runner.run(opts, out_buf, diff_buf);
    check(res.has_value(), "ProcessRunner::run failed");
    check((res->flags & axiom::wire::EXEC_RESP_TIMED_OUT) != 0,
          "Expected EXEC_RESP_TIMED_OUT flag");
    check(res->exit_code != 0, "Expected non-zero exit code on timeout");

    std::printf("[PASS] test_timeout_enforcement passed\n");
}

/// Runs all process runner integration tests.
auto main() -> int {
    std::printf("[integration-test] Starting ProcessRunner test suite\n");
    test_basic_exec();
    test_ansi_stripping();
    test_file_diff_extraction();
    test_timeout_enforcement();
    std::printf("[integration-test] All ProcessRunner integration tests passed\n");
    return EXIT_SUCCESS;
}
