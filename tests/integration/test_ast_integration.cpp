#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "axiom/ast.hpp"

namespace {

void check(bool condition, const char* msg) {
    if (!condition) {
        std::fprintf(stderr, "Assertion failed: %s\n", msg);
        std::abort();
    }
}

void test_cpp_ast_integration(axiom::ast::AstEngine& engine) {
    constexpr std::string_view cpp_source = "#include <iostream>\n"
                                            "class NetworkEngine {\n"
                                            "public:\n"
                                            "    void start() {\n"
                                            "        running = true;\n"
                                            "    }\n"
                                            "    void stop();\n"
                                            "};\n"
                                            "void NetworkEngine::stop() {\n"
                                            "    running = false;\n"
                                            "}\n"
                                            "int main() {\n"
                                            "    return 0;\n"
                                            "}\n";

    axiom::FixedVector<axiom::ast::SymbolInfo, axiom::ast::MAX_SYMBOLS> symbols{};
    auto ext_res = engine.extract_symbols(cpp_source, axiom::ast::Lang::Cpp, symbols);
    check(ext_res.has_value(), "C++ symbol extraction failed");
    check(symbols.size() >= 5, "Expected at least 5 C++ symbols");

    std::array<char, 2048> format_buf{};
    auto fmt_res = axiom::ast::AstEngine::format_symbols(symbols, format_buf);
    check(fmt_res.has_value(), "Format symbols failed");
    std::string_view formatted(format_buf.data(), *fmt_res);
    check(formatted.find("class NetworkEngine") != std::string_view::npos,
          "Missing class NetworkEngine");
    check(formatted.find("method NetworkEngine::start") != std::string_view::npos,
          "Missing start method");
    check(formatted.find("function main") != std::string_view::npos, "Missing main function");

    // Test slicing
    axiom::ast::SymbolSlice slice{};
    auto slice_res =
        engine.slice_symbol(cpp_source, axiom::ast::Lang::Cpp, "NetworkEngine::start", slice);
    check(slice_res.has_value(), "Slice NetworkEngine::start failed");
    check(slice.start_line == 4, "Start line mismatch for start()");
    check(slice.end_line == 6, "End line mismatch for start()");
    check(slice.content.find("running = true;") != std::string_view::npos,
          "Slice content mismatch");

    // Test patching
    std::array<char, 4096> patch_buf{};
    axiom::ast::PatchResult patch_res{};
    constexpr std::string_view replacement =
        "    void start() {\n        running = true;\n        initialized = true;\n    }";
    auto p_res = engine.patch_symbol(cpp_source, axiom::ast::Lang::Cpp, "NetworkEngine::start",
                                     replacement, patch_buf, patch_res);
    check(p_res.has_value(), "Patch symbol failed");
    std::string_view patched_source(patch_buf.data(), *p_res);
    check(patched_source.find("initialized = true;") != std::string_view::npos,
          "Patched content missing replacement");
    check(patch_res.old_start_line == 4, "Old start line mismatch");
    check(patch_res.old_end_line == 6, "Old end line mismatch");
    check(patch_res.new_end_line == 7, "New end line mismatch");
}

void test_rust_ast_integration(axiom::ast::AstEngine& engine) {
    constexpr std::string_view rust_source = "use std::sync::Arc;\n"
                                             "pub struct AppConfig {\n"
                                             "    port: u16,\n"
                                             "}\n"
                                             "impl AppConfig {\n"
                                             "    pub fn new(port: u16) -> Self {\n"
                                             "        AppConfig { port }\n"
                                             "    }\n"
                                             "}\n"
                                             "fn launch() {}\n";

    axiom::FixedVector<axiom::ast::SymbolInfo, axiom::ast::MAX_SYMBOLS> symbols{};
    auto ext_res = engine.extract_symbols(rust_source, axiom::ast::Lang::Rust, symbols);
    check(ext_res.has_value(), "Rust symbol extraction failed");
    check(symbols.size() >= 4, "Expected at least 4 Rust symbols");

    axiom::ast::SymbolSlice slice{};
    auto slice_res =
        engine.slice_symbol(rust_source, axiom::ast::Lang::Rust, "AppConfig::new", slice);
    check(slice_res.has_value(), "Slice AppConfig::new failed");
    check(slice.content.find("AppConfig { port }") != std::string_view::npos,
          "Slice content mismatch");

    std::array<char, 4096> patch_buf{};
    axiom::ast::PatchResult patch_res{};
    constexpr std::string_view replacement =
        "    pub fn new(port: u16) -> Self {\n        AppConfig { port: port + 1 }\n    }";
    auto p_res = engine.patch_symbol(rust_source, axiom::ast::Lang::Rust, "AppConfig::new",
                                     replacement, patch_buf, patch_res);
    check(p_res.has_value(), "Rust patch symbol failed");
    std::string_view patched(patch_buf.data(), *p_res);
    check(patched.find("port: port + 1") != std::string_view::npos,
          "Patched content missing replacement");
}

void test_go_ast_integration(axiom::ast::AstEngine& engine) {
    constexpr std::string_view go_source =
        "package service\n"
        "import \"context\"\n"
        "type Handler struct {\n"
        "    id int\n"
        "}\n"
        "func (h *Handler) Execute(ctx context.Context) error {\n"
        "    return nil\n"
        "}\n"
        "func DefaultConfig() Handler {\n"
        "    return Handler{id: 1}\n"
        "}\n";

    axiom::FixedVector<axiom::ast::SymbolInfo, axiom::ast::MAX_SYMBOLS> symbols{};
    auto ext_res = engine.extract_symbols(go_source, axiom::ast::Lang::Go, symbols);
    check(ext_res.has_value(), "Go symbol extraction failed");
    check(symbols.size() >= 4, "Expected at least 4 Go symbols");

    axiom::ast::SymbolSlice slice{};
    auto slice_res = engine.slice_symbol(go_source, axiom::ast::Lang::Go, "Execute", slice);
    check(slice_res.has_value(), "Slice Go Execute failed");
    check(slice.content.find("return nil") != std::string_view::npos, "Slice content mismatch");
}

void test_c_ast_integration(axiom::ast::AstEngine& engine) {
    constexpr std::string_view c_source = "#include <stdio.h>\n"
                                          "struct BufferHeader {\n"
                                          "    int capacity;\n"
                                          "};\n"
                                          "int process_data(int val) {\n"
                                          "    return val * 2;\n"
                                          "}\n";

    axiom::FixedVector<axiom::ast::SymbolInfo, axiom::ast::MAX_SYMBOLS> symbols{};
    auto ext_res = engine.extract_symbols(c_source, axiom::ast::Lang::C, symbols);
    check(ext_res.has_value(), "C symbol extraction failed");
    check(symbols.size() >= 3, "Expected at least 3 C symbols");

    axiom::ast::SymbolSlice slice{};
    auto slice_res = engine.slice_symbol(c_source, axiom::ast::Lang::C, "process_data", slice);
    check(slice_res.has_value(), "Slice C process_data failed");
    check(slice.content.find("val * 2") != std::string_view::npos, "C slice content mismatch");
}

} // namespace

/// Runs integration tests verifying multi-language AST symbolic extraction, slicing, and patching.
auto main() -> int {
    axiom::ast::AstEngine engine{};
    test_cpp_ast_integration(engine);
    test_rust_ast_integration(engine);
    test_go_ast_integration(engine);
    test_c_ast_integration(engine);

    std::printf("[integration-test] AST engine multi-language integration tests passed\n");
    return EXIT_SUCCESS;
}
