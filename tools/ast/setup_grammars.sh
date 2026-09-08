#!/bin/sh
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VENDOR_DIR="${REPO_ROOT}/vendor/grammars"

mkdir -p "${VENDOR_DIR}"

stage_grammar() {
    lang="$1"
    system_pattern="$2"
    repo_url="$3"
    git_tag="$4"

    mkdir -p "${VENDOR_DIR}/${lang}"

    for p in ${system_pattern}; do
        if [ -d "$p" ]; then
            cp -r "$p"/* "${VENDOR_DIR}/${lang}/" 2>/dev/null || true
            break
        fi
    done

    needs_fetch=0
    if [ ! -f "${VENDOR_DIR}/${lang}/parser.c" ]; then
        needs_fetch=1
    elif grep -q "LANGUAGE_VERSION 15" "${VENDOR_DIR}/${lang}/parser.c"; then
        needs_fetch=1
    fi

    if [ "${needs_fetch}" -eq 1 ]; then
        echo "[grammars] Fetching ${lang} grammar (${git_tag}) from ${repo_url}..."
        rm -rf "${VENDOR_DIR}/${lang}"
        mkdir -p "${VENDOR_DIR}/${lang}"
        tmp_dir="$(mktemp -d)"
        git clone --depth 1 --branch "${git_tag}" "${repo_url}" "${tmp_dir}"
        cp -r "${tmp_dir}"/src/* "${VENDOR_DIR}/${lang}/"
        rm -rf "${tmp_dir}"
    fi
}

stage_grammar "c" "/usr/src/tree-sitter/c/*/parser/src" "https://github.com/tree-sitter/tree-sitter-c.git" "v0.21.4"
stage_grammar "cpp" "/usr/src/tree-sitter/cpp/*/parser/src" "https://github.com/tree-sitter/tree-sitter-cpp.git" "v0.22.0"
stage_grammar "go" "/usr/src/tree-sitter/go/*/parser/src" "https://github.com/tree-sitter/tree-sitter-go.git" "v0.21.0"
stage_grammar "rust" "/usr/src/tree-sitter/rust-orchard/*/parser/src" "https://github.com/tree-sitter/tree-sitter-rust.git" "v0.21.2"

if [ -f "${VENDOR_DIR}/rust/parser.c" ]; then
    if grep -q "tree_sitter_rust(" "${VENDOR_DIR}/rust/parser.c" && ! grep -q "tree_sitter_rust_orchard" "${VENDOR_DIR}/rust/parser.c"; then
        cat << 'EOF' >> "${VENDOR_DIR}/rust/parser.c"

const TSLanguage *tree_sitter_rust_orchard(void) {
    return tree_sitter_rust();
}
EOF
    fi
fi

echo "[grammars] Tree-sitter grammars staged in ${VENDOR_DIR}"
