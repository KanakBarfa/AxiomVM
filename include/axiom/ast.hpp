#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>

#include "axiom/common.hpp"
#include "axiom/fixed_vector.hpp"

extern "C" {
/// Tree-sitter language entry point for C.
const TSLanguage* tree_sitter_c(void);

/// Tree-sitter language entry point for C++.
const TSLanguage* tree_sitter_cpp(void);

/// Tree-sitter language entry point for Go.
const TSLanguage* tree_sitter_go(void);

/// Tree-sitter language entry point for Rust.
const TSLanguage* tree_sitter_rust_orchard(void);

/// Tree-sitter language entry point for Python.
const TSLanguage* tree_sitter_python(void);

/// Tree-sitter language entry point for TypeScript.
const TSLanguage* tree_sitter_typescript(void);
}

namespace axiom::ast {

/// Supported source code languages for AST parsing.
enum class Lang : uint8_t {
    Unknown = 0,
    C,
    Cpp,
    Go,
    Rust,
    Python,
    TypeScript,
    JavaScript,
};

/// High-level symbolic category extracted from source AST.
enum class SymbolKind : uint8_t {
    Unknown = 0,
    Function,
    Method,
    Class,
    Struct,
    Enum,
    Interface,
    TypeAlias,
    Import,
};

/// Maximum symbol capacity extracted per source file.
inline constexpr size_t MAX_SYMBOLS = 256;

/// Maximum length of symbol name stored without dynamic allocation.
inline constexpr size_t MAX_SYMBOL_NAME_LEN = 96;

/// String representation of symbolic category.
[[nodiscard]] constexpr auto symbol_kind_to_string(SymbolKind kind) noexcept -> std::string_view {
    switch (kind) {
    case SymbolKind::Function:
        return "function";
    case SymbolKind::Method:
        return "method";
    case SymbolKind::Class:
        return "class";
    case SymbolKind::Struct:
        return "struct";
    case SymbolKind::Enum:
        return "enum";
    case SymbolKind::Interface:
        return "interface";
    case SymbolKind::TypeAlias:
        return "type";
    case SymbolKind::Import:
        return "import";
    default:
        return "unknown";
    }
}

/// Detects target language from file path extension.
[[nodiscard]] constexpr auto detect_language(std::string_view path) noexcept -> Lang {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return Lang::Unknown;
    }
    auto ext = path.substr(dot);
    if (ext == ".c" || ext == ".h") {
        return Lang::C;
    }
    if (ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".cxx" || ext == ".hh") {
        return Lang::Cpp;
    }
    if (ext == ".go") {
        return Lang::Go;
    }
    if (ext == ".rs") {
        return Lang::Rust;
    }
    if (ext == ".py") {
        return Lang::Python;
    }
    if (ext == ".ts" || ext == ".tsx") {
        return Lang::TypeScript;
    }
    if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs") {
        return Lang::JavaScript;
    }
    return Lang::Unknown;
}

/// Metadata describing a discovered symbol declaration.
struct SymbolInfo {
    SymbolKind kind{SymbolKind::Unknown};
    std::array<char, MAX_SYMBOL_NAME_LEN> name{};
    uint16_t name_len{0};
    uint32_t start_line{0};
    uint32_t end_line{0};
    uint32_t start_byte{0};
    uint32_t end_byte{0};

    /// Returns string view of the symbol name.
    [[nodiscard]] constexpr auto symbol_name() const noexcept -> std::string_view {
        return {name.data(), name_len};
    }
};

/// Slice metadata and view referencing source buffer directly.
struct SymbolSlice {
    uint32_t start_line{0};
    uint32_t end_line{0};
    std::string_view content{};
};

/// Line boundary metadata updated after applying a structural patch.
struct PatchResult {
    uint32_t old_start_line{0};
    uint32_t old_end_line{0};
    uint32_t new_end_line{0};
};

/// Embedded AST engine wrapping Tree-sitter for zero-allocation symbolic parsing.
class AstEngine {
public:
    /// Initializes Tree-sitter parser instance.
    AstEngine() noexcept { parser_ = ts_parser_new(); }

    /// Releases Tree-sitter parser resources.
    ~AstEngine() noexcept {
        if (parser_ != nullptr) {
            ts_parser_delete(parser_);
            parser_ = nullptr;
        }
    }

    AstEngine(const AstEngine&) = delete;
    auto operator=(const AstEngine&) -> AstEngine& = delete;

    AstEngine(AstEngine&& other) noexcept : parser_(other.parser_) { other.parser_ = nullptr; }

    auto operator=(AstEngine&& other) noexcept -> AstEngine& {
        if (this != &other) {
            if (parser_ != nullptr) {
                ts_parser_delete(parser_);
            }
            parser_ = other.parser_;
            other.parser_ = nullptr;
        }
        return *this;
    }

    /// Resolves active Tree-sitter language grammar pointer.
    [[nodiscard]] static auto get_language(Lang lang) noexcept -> const TSLanguage* {
        switch (lang) {
        case Lang::C:
            return tree_sitter_c();
        case Lang::Cpp:
            return tree_sitter_cpp();
        case Lang::Go:
            return tree_sitter_go();
        case Lang::Rust:
            return tree_sitter_rust_orchard();
        case Lang::Python:
            return tree_sitter_python();
        case Lang::TypeScript:
        case Lang::JavaScript:
            return tree_sitter_typescript();
        default:
            return nullptr;
        }
    }

    /// Extracts all top-level and member symbol declarations from source code.
    auto extract_symbols(std::string_view source, Lang lang,
                         FixedVector<SymbolInfo, MAX_SYMBOLS>& out) noexcept -> Result<size_t> {
        if (parser_ == nullptr) {
            return std::unexpected(SystemError::AstError);
        }

        const TSLanguage* ts_lang = get_language(lang);
        if (ts_lang == nullptr) {
            return std::unexpected(SystemError::InvalidArgument);
        }

        if (!ts_parser_set_language(parser_, ts_lang)) {
            return std::unexpected(SystemError::AstError);
        }

        TSTree* tree = ts_parser_parse_string(parser_, nullptr, source.data(),
                                              static_cast<uint32_t>(source.size()));
        if (tree == nullptr) {
            return std::unexpected(SystemError::AstError);
        }

        out.clear();
        TSNode root = ts_tree_root_node(tree);
        collect_symbols_recursive(root, source, lang, out, "");
        ts_tree_delete(tree);

        return out.size();
    }

    /// Extracts source slice for a target symbol name.
    auto slice_symbol(std::string_view source, Lang lang, std::string_view symbol_name,
                      SymbolSlice& out_slice) noexcept -> Result<void> {
        FixedVector<SymbolInfo, MAX_SYMBOLS> symbols{};
        auto res = extract_symbols(source, lang, symbols);
        if (!res) {
            return std::unexpected(res.error());
        }

        const SymbolInfo* matched = find_symbol(symbols, symbol_name);
        if (matched == nullptr) {
            return std::unexpected(SystemError::NotFound);
        }

        out_slice.start_line = matched->start_line;
        out_slice.end_line = matched->end_line;
        size_t len = matched->end_byte - matched->start_byte;
        out_slice.content = source.substr(matched->start_byte, len);
        return {};
    }

    /// Applies replacement string to a target symbol and writes modified file into dest_buf.
    auto patch_symbol(std::string_view source, Lang lang, std::string_view symbol_name,
                      std::string_view replacement, std::span<char> dest_buf,
                      PatchResult& out_result) noexcept -> Result<size_t> {
        FixedVector<SymbolInfo, MAX_SYMBOLS> symbols{};
        auto res = extract_symbols(source, lang, symbols);
        if (!res) {
            return std::unexpected(res.error());
        }

        const SymbolInfo* matched = find_symbol(symbols, symbol_name);
        if (matched == nullptr) {
            return std::unexpected(SystemError::NotFound);
        }

        size_t prefix_len = matched->start_byte;
        size_t suffix_len = source.size() - matched->end_byte;
        size_t total_len = prefix_len + replacement.size() + suffix_len;

        if (total_len > dest_buf.size()) {
            return std::unexpected(SystemError::BufferOverflow);
        }

        if (prefix_len > 0) {
            std::memcpy(dest_buf.data(), source.data(), prefix_len);
        }
        if (!replacement.empty()) {
            std::memcpy(dest_buf.data() + prefix_len, replacement.data(), replacement.size());
        }
        if (suffix_len > 0) {
            std::memcpy(dest_buf.data() + prefix_len + replacement.size(),
                        source.data() + matched->end_byte, suffix_len);
        }

        uint32_t rep_newlines = 0;
        for (char c : replacement) {
            if (c == '\n') {
                ++rep_newlines;
            }
        }

        out_result.old_start_line = matched->start_line;
        out_result.old_end_line = matched->end_line;
        out_result.new_end_line = matched->start_line + rep_newlines;

        return total_len;
    }

    /// Formats extracted symbols into token-dense line entries.
    static auto format_symbols(const FixedVector<SymbolInfo, MAX_SYMBOLS>& symbols,
                               std::span<char> out_buf) noexcept -> Result<size_t> {
        size_t offset = 0;
        for (const auto& sym : symbols) {
            std::string_view kind_str = symbol_kind_to_string(sym.kind);
            std::string_view name_str = sym.symbol_name();

            size_t needed = kind_str.size() + 1 + name_str.size() + 32;
            if (offset + needed >= out_buf.size()) {
                return std::unexpected(SystemError::BufferOverflow);
            }

            std::memcpy(out_buf.data() + offset, kind_str.data(), kind_str.size());
            offset += kind_str.size();
            out_buf[offset++] = ' ';

            std::memcpy(out_buf.data() + offset, name_str.data(), name_str.size());
            offset += name_str.size();

            std::string_view l_start = " [L";
            std::memcpy(out_buf.data() + offset, l_start.data(), l_start.size());
            offset += l_start.size();

            auto [ptr1, ec1] = std::to_chars(out_buf.data() + offset,
                                             out_buf.data() + out_buf.size(), sym.start_line);
            if (ec1 != std::errc()) {
                return std::unexpected(SystemError::BufferOverflow);
            }
            offset = static_cast<size_t>(ptr1 - out_buf.data());

            std::string_view l_mid = "-L";
            std::memcpy(out_buf.data() + offset, l_mid.data(), l_mid.size());
            offset += l_mid.size();

            auto [ptr2, ec2] = std::to_chars(out_buf.data() + offset,
                                             out_buf.data() + out_buf.size(), sym.end_line);
            if (ec2 != std::errc()) {
                return std::unexpected(SystemError::BufferOverflow);
            }
            offset = static_cast<size_t>(ptr2 - out_buf.data());

            std::string_view l_end = "]\n";
            std::memcpy(out_buf.data() + offset, l_end.data(), l_end.size());
            offset += l_end.size();
        }

        return offset;
    }

private:
    TSParser* parser_{nullptr};

    /// Extracts node text slice from source buffer.
    [[nodiscard]] static auto node_text(TSNode node, std::string_view src) noexcept
        -> std::string_view {
        if (ts_node_is_null(node)) {
            return "";
        }
        uint32_t sb = ts_node_start_byte(node);
        uint32_t eb = ts_node_end_byte(node);
        if (sb >= src.size() || eb > src.size() || sb >= eb) {
            return "";
        }
        return src.substr(sb, eb - sb);
    }

    /// Finds first descendant matching any of the candidate node types.
    [[nodiscard]] static auto find_descendant(TSNode node,
                                              std::span<const std::string_view> types) noexcept
        -> TSNode {
        if (ts_node_is_null(node)) {
            return node;
        }
        std::string_view nt = ts_node_type(node);
        for (const auto& t : types) {
            if (nt == t) {
                return node;
            }
        }
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode res = find_descendant(ts_node_child(node, i), types);
            if (!ts_node_is_null(res)) {
                return res;
            }
        }
        return TSNode{};
    }

    /// Copies string and bounds into a SymbolInfo struct.
    static void record_symbol(SymbolKind k, std::string_view name_str, TSNode sym_node,
                              FixedVector<SymbolInfo, MAX_SYMBOLS>& out,
                              std::string_view parent_scope) noexcept {
        if (name_str.empty() || out.full()) {
            return;
        }

        SymbolInfo info{};
        info.kind = k;
        size_t off = 0;
        if (!parent_scope.empty() && k == SymbolKind::Method) {
            size_t copy_len = std::min(parent_scope.size(), MAX_SYMBOL_NAME_LEN - 3);
            std::memcpy(info.name.data(), parent_scope.data(), copy_len);
            off += copy_len;
            info.name[off++] = ':';
            info.name[off++] = ':';
        }

        size_t remaining = MAX_SYMBOL_NAME_LEN - 1 - off;
        size_t copy_len = std::min(name_str.size(), remaining);
        std::memcpy(info.name.data() + off, name_str.data(), copy_len);
        off += copy_len;
        info.name_len = static_cast<uint16_t>(off);

        TSPoint sp = ts_node_start_point(sym_node);
        TSPoint ep = ts_node_end_point(sym_node);
        info.start_line = sp.row + 1;
        info.end_line = ep.row + 1;
        info.start_byte = ts_node_start_byte(sym_node);
        info.end_byte = ts_node_end_byte(sym_node);

        (void)out.push_back(info);
    }

    /// Recursively traverses AST nodes and populates symbol collection.
    static void collect_symbols_recursive(TSNode node, std::string_view src, Lang lang,
                                          FixedVector<SymbolInfo, MAX_SYMBOLS>& out,
                                          std::string_view parent_scope) noexcept {
        if (ts_node_is_null(node) || out.full()) {
            return;
        }
        std::string_view type = ts_node_type(node);

        if (lang == Lang::Rust) {
            if (type == "function_item") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                SymbolKind k = parent_scope.empty() ? SymbolKind::Function : SymbolKind::Method;
                record_symbol(k, node_text(name_node, src), node, out, parent_scope);
                return;
            }
            if (type == "struct_item") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::Struct, node_text(name_node, src), node, out,
                              parent_scope);
                return;
            }
            if (type == "enum_item") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::Enum, node_text(name_node, src), node, out, parent_scope);
                return;
            }
            if (type == "trait_item") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::Interface, node_text(name_node, src), node, out,
                              parent_scope);
                return;
            }
            if (type == "use_declaration") {
                std::string_view full_text = node_text(node, src);
                while (!full_text.empty() &&
                       (full_text.back() == '\n' || full_text.back() == '\r' ||
                        full_text.back() == ';')) {
                    full_text.remove_suffix(1);
                }
                record_symbol(SymbolKind::Import, full_text, node, out, parent_scope);
                return;
            }
            if (type == "impl_item") {
                TSNode type_node = ts_node_child_by_field_name(node, "type", 4);
                std::string_view type_name = node_text(type_node, src);
                TSNode body = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_named_child_count(body);
                    for (uint32_t i = 0; i < count; ++i) {
                        collect_symbols_recursive(ts_node_named_child(body, i), src, lang, out,
                                                  type_name);
                    }
                }
                return;
            }
        } else if (lang == Lang::Go) {
            if (type == "function_declaration") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::Function, node_text(name_node, src), node, out,
                              parent_scope);
                return;
            }
            if (type == "method_declaration") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::Method, node_text(name_node, src), node, out,
                              parent_scope);
                return;
            }
            if (type == "type_declaration") {
                uint32_t count = ts_node_named_child_count(node);
                for (uint32_t i = 0; i < count; ++i) {
                    TSNode spec = ts_node_named_child(node, i);
                    if (std::string_view(ts_node_type(spec)) == "type_spec") {
                        TSNode name_node = ts_node_child_by_field_name(spec, "name", 4);
                        TSNode type_node = ts_node_child_by_field_name(spec, "type", 4);
                        std::string_view tt = ts_node_type(type_node);
                        SymbolKind k = SymbolKind::TypeAlias;
                        if (tt == "struct_type") {
                            k = SymbolKind::Struct;
                        } else if (tt == "interface_type") {
                            k = SymbolKind::Interface;
                        }
                        record_symbol(k, node_text(name_node, src), spec, out, parent_scope);
                    }
                }
                return;
            }
            if (type == "import_declaration") {
                std::string_view full_text = node_text(node, src);
                while (!full_text.empty() &&
                       (full_text.back() == '\n' || full_text.back() == '\r')) {
                    full_text.remove_suffix(1);
                }
                record_symbol(SymbolKind::Import, full_text, node, out, parent_scope);
                return;
            }
        } else if (lang == Lang::C || lang == Lang::Cpp) {
            if (type == "function_definition") {
                TSNode decl = ts_node_child_by_field_name(node, "declarator", 10);
                std::array<std::string_view, 4> targets = {
                    "identifier", "field_identifier", "qualified_identifier", "destructor_name"};
                TSNode id = find_descendant(decl, targets);
                SymbolKind k = (!parent_scope.empty() ||
                                std::string_view(ts_node_type(id)) == "qualified_identifier")
                                   ? SymbolKind::Method
                                   : SymbolKind::Function;
                record_symbol(k, node_text(id, src), node, out, parent_scope);
                return;
            }
            if (type == "class_specifier" || type == "struct_specifier") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                std::string_view class_name = node_text(name_node, src);
                SymbolKind k = (type == "class_specifier") ? SymbolKind::Class : SymbolKind::Struct;
                record_symbol(k, class_name, node, out, parent_scope);

                TSNode body = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_named_child_count(body);
                    for (uint32_t i = 0; i < count; ++i) {
                        collect_symbols_recursive(ts_node_named_child(body, i), src, lang, out,
                                                  class_name);
                    }
                }
                return;
            }
            if (type == "enum_specifier") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::Enum, node_text(name_node, src), node, out, parent_scope);
                return;
            }
            if (type == "type_definition") {
                TSNode decl = ts_node_child_by_field_name(node, "declarator", 10);
                std::array<std::string_view, 2> targets = {"type_identifier", "identifier"};
                TSNode id = find_descendant(decl, targets);
                record_symbol(SymbolKind::TypeAlias, node_text(id, src), node, out, parent_scope);
                return;
            }
            if (type == "preproc_include") {
                std::string_view full_text = node_text(node, src);
                while (!full_text.empty() &&
                       (full_text.back() == '\n' || full_text.back() == '\r')) {
                    full_text.remove_suffix(1);
                }
                record_symbol(SymbolKind::Import, full_text, node, out, parent_scope);
                return;
            }
            if (type == "template_declaration") {
                uint32_t count = ts_node_named_child_count(node);
                for (uint32_t i = 0; i < count; ++i) {
                    TSNode child = ts_node_named_child(node, i);
                    std::string_view ct = ts_node_type(child);
                    if (ct == "function_definition" || ct == "class_specifier" ||
                        ct == "struct_specifier") {
                        collect_symbols_recursive(child, src, lang, out, parent_scope);
                    }
                }
                return;
            }
            if (type == "declaration") {
                uint32_t count = ts_node_named_child_count(node);
                for (uint32_t i = 0; i < count; ++i) {
                    TSNode child = ts_node_named_child(node, i);
                    std::string_view ct = ts_node_type(child);
                    if (ct == "class_specifier" || ct == "struct_specifier" ||
                        ct == "enum_specifier") {
                        collect_symbols_recursive(child, src, lang, out, parent_scope);
                    }
                }
                return;
            }
        } else if (lang == Lang::Python) {
            if (type == "function_definition") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                SymbolKind k = parent_scope.empty() ? SymbolKind::Function : SymbolKind::Method;
                record_symbol(k, node_text(name_node, src), node, out, parent_scope);
                return;
            }
            if (type == "class_definition") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                std::string_view class_name = node_text(name_node, src);
                record_symbol(SymbolKind::Class, class_name, node, out, parent_scope);

                TSNode body = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_named_child_count(body);
                    for (uint32_t i = 0; i < count; ++i) {
                        collect_symbols_recursive(ts_node_named_child(body, i), src, lang, out,
                                                  class_name);
                    }
                }
                return;
            }
            if (type == "import_statement" || type == "import_from_statement") {
                std::string_view full_text = node_text(node, src);
                while (!full_text.empty() &&
                       (full_text.back() == '\n' || full_text.back() == '\r')) {
                    full_text.remove_suffix(1);
                }
                record_symbol(SymbolKind::Import, full_text, node, out, parent_scope);
                return;
            }
        } else if (lang == Lang::TypeScript || lang == Lang::JavaScript) {
            if (type == "function_declaration" || type == "function_signature") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                SymbolKind k = parent_scope.empty() ? SymbolKind::Function : SymbolKind::Method;
                record_symbol(k, node_text(name_node, src), node, out, parent_scope);
                return;
            }
            if (type == "method_definition" || type == "method_signature") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::Method, node_text(name_node, src), node, out,
                              parent_scope);
                return;
            }
            if (type == "class_declaration" || type == "class") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                std::string_view class_name = node_text(name_node, src);
                record_symbol(SymbolKind::Class, class_name, node, out, parent_scope);

                TSNode body = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(body)) {
                    uint32_t count = ts_node_named_child_count(body);
                    for (uint32_t i = 0; i < count; ++i) {
                        collect_symbols_recursive(ts_node_named_child(body, i), src, lang, out,
                                                  class_name);
                    }
                }
                return;
            }
            if (type == "interface_declaration") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::Interface, node_text(name_node, src), node, out,
                              parent_scope);
                return;
            }
            if (type == "type_alias_declaration") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::TypeAlias, node_text(name_node, src), node, out,
                              parent_scope);
                return;
            }
            if (type == "enum_declaration") {
                TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
                record_symbol(SymbolKind::Enum, node_text(name_node, src), node, out, parent_scope);
                return;
            }
            if (type == "import_statement") {
                std::string_view full_text = node_text(node, src);
                while (!full_text.empty() &&
                       (full_text.back() == '\n' || full_text.back() == '\r' ||
                        full_text.back() == ';')) {
                    full_text.remove_suffix(1);
                }
                record_symbol(SymbolKind::Import, full_text, node, out, parent_scope);
                return;
            }
            if (type == "export_statement") {
                TSNode decl = ts_node_child_by_field_name(node, "declaration", 11);
                if (!ts_node_is_null(decl)) {
                    collect_symbols_recursive(decl, src, lang, out, parent_scope);
                    return;
                }
            }
        }

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i) {
            collect_symbols_recursive(ts_node_named_child(node, i), src, lang, out, parent_scope);
        }
    }

    /// Matches symbol by exact name or scoped suffix.
    [[nodiscard]] static auto find_symbol(const FixedVector<SymbolInfo, MAX_SYMBOLS>& symbols,
                                          std::string_view query) noexcept -> const SymbolInfo* {
        // Priority 1: Exact match
        for (const auto& sym : symbols) {
            if (sym.symbol_name() == query) {
                return &sym;
            }
        }

        // Priority 2: Match with '.' <-> "::" equivalence
        for (const auto& sym : symbols) {
            std::string_view name = sym.symbol_name();
            size_t i = 0;
            size_t j = 0;
            bool match = true;
            while (i < name.size() && j < query.size()) {
                if (name[i] == ':' && i + 1 < name.size() && name[i + 1] == ':') {
                    if (query[j] == '.') {
                        i += 2;
                        j += 1;
                        continue;
                    }
                    if (j + 1 < query.size() && query[j] == ':' && query[j + 1] == ':') {
                        i += 2;
                        j += 2;
                        continue;
                    }
                    match = false;
                    break;
                }
                if (name[i] != query[j]) {
                    match = false;
                    break;
                }
                ++i;
                ++j;
            }
            if (match && i == name.size() && j == query.size()) {
                return &sym;
            }
        }

        // Priority 3: Scoped suffix match
        for (const auto& sym : symbols) {
            std::string_view name = sym.symbol_name();
            if (name.size() > query.size() + 2) {
                size_t sep_pos = name.size() - query.size() - 2;
                if (name.substr(sep_pos, 2) == "::" && name.substr(sep_pos + 2) == query) {
                    return &sym;
                }
            }
        }

        return nullptr;
    }
};

} // namespace axiom::ast
