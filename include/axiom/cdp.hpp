#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <span>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "axiom/common.hpp"
#include "axiom/fixed_vector.hpp"

namespace axiom::cdp {

inline constexpr size_t MAX_AX_NODES = 256;
inline constexpr size_t MAX_HANDLES = 256;
inline constexpr size_t CDP_BUFFER_SIZE = 128 * 1024;
inline constexpr size_t MAX_TREE_TEXT_SIZE = 64 * 1024;

/// Single accessibility tree node extracted and filtered from CDP.
struct AXNode {
    uint32_t handle{0};
    int32_t backend_dom_node_id{0};
    std::string_view role{};
    std::string_view name{};
    std::string_view value{};
    std::string_view description{};
    uint32_t heading_level{0};
    bool actionable{false};
    bool focused{false};
    bool disabled{false};
};

/// Evaluates whether an ARIA role is actionable by an agent.
[[nodiscard]] constexpr auto is_actionable_role(std::string_view role) noexcept -> bool {
    return role == "button" || role == "link" || role == "textbox" || role == "searchbox" ||
           role == "checkbox" || role == "radio" || role == "combobox" || role == "listbox" ||
           role == "menuitem" || role == "menuitemcheckbox" || role == "menuitemradio" ||
           role == "tab" || role == "switch" || role == "slider" || role == "spinbutton" ||
           role == "treeitem";
}

/// Evaluates whether a role is purely layout or decorative and safe to strip.
[[nodiscard]] constexpr auto is_layout_role(std::string_view role) noexcept -> bool {
    return role == "none" || role == "generic" || role == "genericContainer" ||
           role == "LineBreak" || role == "InlineTextBox" || role == "RootWebArea" ||
           role == "group" || role == "Section" || role == "paragraph" || role == "LayoutTable" ||
           role == "LayoutTableRow" || role == "LayoutTableCell" || role == "Ignored";
}

/// Zero-heap parser and semantic filter for Chromium Accessibility Tree graphs.
class AXTreeFilter {
public:
    /// Extracts a quoted string from a JSON key pattern.
    static auto extract_field_str(std::string_view obj, std::string_view field_name) noexcept
        -> std::string_view {
        size_t pos = obj.find(field_name);
        if (pos == std::string_view::npos) {
            return {};
        }

        pos += field_name.size();
        size_t colon = obj.find(':', pos);
        if (colon == std::string_view::npos) {
            return {};
        }

        size_t after_colon = colon + 1;
        while (after_colon < obj.size() &&
               (obj[after_colon] == ' ' || obj[after_colon] == '\t' || obj[after_colon] == '\n')) {
            ++after_colon;
        }

        if (after_colon < obj.size() && obj[after_colon] == '{') {
            size_t val_key = obj.find("\"value\"", after_colon);
            if (val_key == std::string_view::npos) {
                return {};
            }
            size_t val_colon = obj.find(':', val_key + 7);
            if (val_colon == std::string_view::npos) {
                return {};
            }
            after_colon = val_colon + 1;
            while (after_colon < obj.size() &&
                   (obj[after_colon] == ' ' || obj[after_colon] == '\t')) {
                ++after_colon;
            }
        }

        if (after_colon >= obj.size() || obj[after_colon] != '"') {
            return {};
        }

        size_t str_start = after_colon + 1;
        size_t str_end = str_start;
        bool escape = false;
        while (str_end < obj.size()) {
            if (escape) {
                escape = false;
                ++str_end;
                continue;
            }
            if (obj[str_end] == '\\') {
                escape = true;
                ++str_end;
                continue;
            }
            if (obj[str_end] == '"') {
                break;
            }
            ++str_end;
        }

        return obj.substr(str_start, str_end - str_start);
    }

    /// Extracts an integer value for a target JSON field.
    static auto extract_field_int(std::string_view obj, std::string_view field_name) noexcept
        -> int32_t {
        size_t pos = obj.find(field_name);
        if (pos == std::string_view::npos) {
            return 0;
        }

        pos += field_name.size();
        size_t colon = obj.find(':', pos);
        if (colon == std::string_view::npos) {
            return 0;
        }

        size_t cur = colon + 1;
        while (cur < obj.size() && (obj[cur] == ' ' || obj[cur] == '\t' || obj[cur] == '\n')) {
            ++cur;
        }

        if (cur < obj.size() && obj[cur] == '{') {
            size_t val_key = obj.find("\"value\"", cur);
            if (val_key != std::string_view::npos) {
                size_t val_colon = obj.find(':', val_key + 7);
                if (val_colon != std::string_view::npos) {
                    cur = val_colon + 1;
                    while (cur < obj.size() && (obj[cur] == ' ' || obj[cur] == '\t')) {
                        ++cur;
                    }
                }
            }
        }

        int32_t val = 0;
        auto res = std::from_chars(obj.data() + cur, obj.data() + obj.size(), val);
        if (res.ec == std::errc{}) {
            return val;
        }
        return 0;
    }

    /// Evaluates whether a boolean property is explicitly true in a JSON object.
    static auto extract_field_bool(std::string_view obj, std::string_view field_name) noexcept
        -> bool {
        size_t pos = obj.find(field_name);
        if (pos == std::string_view::npos) {
            return false;
        }

        pos += field_name.size();
        size_t colon = obj.find(':', pos);
        if (colon == std::string_view::npos) {
            return false;
        }

        size_t cur = colon + 1;
        while (cur < obj.size() && (obj[cur] == ' ' || obj[cur] == '\t' || obj[cur] == '\n')) {
            ++cur;
        }

        return obj.substr(cur, 4) == "true";
    }

    /// Formats a string safely into the output buffer with bounds checking.
    static auto append_to_buf(std::span<char> buf, size_t& offset, std::string_view str) noexcept
        -> bool {
        if (offset + str.size() > buf.size()) {
            return false;
        }
        std::memcpy(buf.data() + offset, str.data(), str.size());
        offset += str.size();
        return true;
    }

    /// Appends an integer as text to the output buffer without allocations.
    static auto append_int(std::span<char> buf, size_t& offset, uint32_t val) noexcept -> bool {
        std::array<char, 16> tmp{};
        auto res = std::to_chars(tmp.data(), tmp.data() + tmp.size(), val);
        if (res.ec != std::errc{}) {
            return false;
        }
        std::string_view s(tmp.data(), static_cast<size_t>(res.ptr - tmp.data()));
        return append_to_buf(buf, offset, s);
    }

    /// Parses raw AXTree JSON, filters decorative elements, and formats token-dense tree.
    static auto filter_and_format(std::string_view json,
                                  FixedVector<AXNode, MAX_AX_NODES>& out_nodes,
                                  std::span<char> out_text,
                                  std::span<int32_t> out_handle_map) noexcept -> Result<size_t> {
        out_nodes.clear();

        for (auto& h : out_handle_map) {
            h = 0;
        }

        size_t nodes_pos = json.find("\"nodes\"");
        if (nodes_pos == std::string_view::npos) {
            return std::unexpected(SystemError::ProtocolError);
        }

        size_t bracket = json.find('[', nodes_pos);
        if (bracket == std::string_view::npos) {
            return std::unexpected(SystemError::ProtocolError);
        }

        size_t cur = bracket + 1;
        uint32_t current_handle = 0;
        size_t text_offset = 0;

        while (cur < json.size()) {
            while (cur < json.size() && json[cur] != '{' && json[cur] != ']') {
                ++cur;
            }
            if (cur >= json.size() || json[cur] == ']') {
                break;
            }

            size_t node_start = cur;
            size_t depth = 0;
            bool in_str = false;
            bool esc = false;
            while (cur < json.size()) {
                char c = json[cur];
                if (esc) {
                    esc = false;
                    ++cur;
                    continue;
                }
                if (c == '\\') {
                    esc = true;
                    ++cur;
                    continue;
                }
                if (c == '"') {
                    in_str = !in_str;
                    ++cur;
                    continue;
                }
                if (!in_str) {
                    if (c == '{') {
                        ++depth;
                    } else if (c == '}') {
                        --depth;
                        if (depth == 0) {
                            ++cur;
                            break;
                        }
                    }
                }
                ++cur;
            }

            std::string_view node_json = json.substr(node_start, cur - node_start);

            if (extract_field_bool(node_json, "\"ignored\"")) {
                continue;
            }

            std::string_view role = extract_field_str(node_json, "\"role\"");
            if (is_layout_role(role)) {
                continue;
            }

            std::string_view name = extract_field_str(node_json, "\"name\"");
            std::string_view value = extract_field_str(node_json, "\"value\"");
            std::string_view desc = extract_field_str(node_json, "\"description\"");
            int32_t backend_id = extract_field_int(node_json, "\"backendDOMNodeId\"");
            bool is_action = is_actionable_role(role);

            if (role == "StaticText") {
                if (name.empty()) {
                    continue;
                }
                bool duplicate = false;
                for (size_t i = 0; i < out_nodes.size(); ++i) {
                    if (out_nodes[i].name == name || out_nodes[i].value == name) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    continue;
                }
            }

            AXNode node{
                .handle = 0,
                .backend_dom_node_id = backend_id,
                .role = role,
                .name = name,
                .value = value,
                .description = desc,
                .heading_level = 0,
                .actionable = is_action,
                .focused = extract_field_bool(node_json, "\"focused\""),
                .disabled = extract_field_bool(node_json, "\"disabled\""),
            };

            if (role == "heading") {
                node.heading_level =
                    static_cast<uint32_t>(extract_field_int(node_json, "\"level\""));
                if (node.heading_level == 0) {
                    node.heading_level = 1;
                }
            }

            if (is_action) {
                if (current_handle + 1 < MAX_HANDLES &&
                    current_handle + 1 < out_handle_map.size()) {
                    ++current_handle;
                    node.handle = current_handle;
                    out_handle_map[current_handle] = backend_id;
                }
            }

            if (!out_nodes.push_back(node)) {
                break;
            }

            if (node.actionable) {
                (void)append_to_buf(out_text, text_offset, "[@");
                (void)append_int(out_text, text_offset, node.handle);
                (void)append_to_buf(out_text, text_offset, "] ");
                (void)append_to_buf(out_text, text_offset, node.role);
                if (!node.name.empty()) {
                    (void)append_to_buf(out_text, text_offset, " \"");
                    (void)append_to_buf(out_text, text_offset, node.name);
                    (void)append_to_buf(out_text, text_offset, "\"");
                }
                if (!node.value.empty()) {
                    (void)append_to_buf(out_text, text_offset, " (value=\"");
                    (void)append_to_buf(out_text, text_offset, node.value);
                    (void)append_to_buf(out_text, text_offset, "\")");
                }
                if (!node.description.empty()) {
                    (void)append_to_buf(out_text, text_offset, " (desc=\"");
                    (void)append_to_buf(out_text, text_offset, node.description);
                    (void)append_to_buf(out_text, text_offset, "\")");
                }
                (void)append_to_buf(out_text, text_offset, "\n");
            } else if (node.role == "heading") {
                (void)append_to_buf(out_text, text_offset, "[heading:");
                (void)append_int(out_text, text_offset, node.heading_level);
                (void)append_to_buf(out_text, text_offset, "] \"");
                (void)append_to_buf(out_text, text_offset, node.name);
                (void)append_to_buf(out_text, text_offset, "\"\n");
            } else if (!node.name.empty()) {
                (void)append_to_buf(out_text, text_offset, "text: \"");
                (void)append_to_buf(out_text, text_offset, node.name);
                (void)append_to_buf(out_text, text_offset, "\"\n");
            }
        }

        return text_offset;
    }
};

/// Orchestrates headless Chromium lifecycle and bidirectional CDP pipe transport.
class CdpBridge {
public:
    CdpBridge() noexcept = default;

    ~CdpBridge() noexcept { stop(); }

    CdpBridge(const CdpBridge&) = delete;
    auto operator=(const CdpBridge&) -> CdpBridge& = delete;

    CdpBridge(CdpBridge&& other) noexcept
        : to_chrome_fd_(std::exchange(other.to_chrome_fd_, -1)),
          from_chrome_fd_(std::exchange(other.from_chrome_fd_, -1)),
          child_pid_(std::exchange(other.child_pid_, -1)), next_req_id_(other.next_req_id_),
          started_(std::exchange(other.started_, false)), session_id_(other.session_id_),
          handle_map_(other.handle_map_) {
        other.session_id_.fill('\0');
        other.handle_map_.fill(0);
    }

    auto operator=(CdpBridge&& other) noexcept -> CdpBridge& {
        if (this != &other) {
            stop();
            to_chrome_fd_ = std::exchange(other.to_chrome_fd_, -1);
            from_chrome_fd_ = std::exchange(other.from_chrome_fd_, -1);
            child_pid_ = std::exchange(other.child_pid_, -1);
            next_req_id_ = other.next_req_id_;
            started_ = std::exchange(other.started_, false);
            session_id_ = other.session_id_;
            handle_map_ = other.handle_map_;
            other.session_id_.fill('\0');
            other.handle_map_.fill(0);
        }
        return *this;
    }

    /// Configures bridge with existing pipe descriptors for hermetic integration testing.
    auto init_from_fds(int to_chrome_fd, int from_chrome_fd) noexcept -> Result<void> {
        stop();
        to_chrome_fd_ = to_chrome_fd;
        from_chrome_fd_ = from_chrome_fd;
        started_ = true;
        return {};
    }

    /// Spawns headless Chromium with remote debugging pipes and initializes CDP session.
    auto start(std::string_view initial_url = "about:blank") noexcept -> Result<void> {
        if (started_) {
            return {};
        }

        const char* chrome_path = nullptr;
        if (access("/usr/lib/chromium/chromium", X_OK) == 0) {
            chrome_path = "/usr/lib/chromium/chromium";
        } else if (access("/usr/bin/chromium", X_OK) == 0) {
            chrome_path = "/usr/bin/chromium";
        } else if (access("/usr/bin/chromium-browser", X_OK) == 0) {
            chrome_path = "/usr/bin/chromium-browser";
        } else {
            return std::unexpected(SystemError::NotFound);
        }

        int to_chrome[2];
        int from_chrome[2];
        if (pipe2(to_chrome, O_CLOEXEC) < 0 || pipe2(from_chrome, O_CLOEXEC) < 0) {
            return std::unexpected(SystemError::IoError);
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(to_chrome[0]);
            close(to_chrome[1]);
            close(from_chrome[0]);
            close(from_chrome[1]);
            return std::unexpected(SystemError::ProcessSpawnError);
        }

        if (pid == 0) {
            int in_fd = fcntl(to_chrome[0], F_DUPFD_CLOEXEC, 10);
            int out_fd = fcntl(from_chrome[1], F_DUPFD_CLOEXEC, 11);

            close(to_chrome[0]);
            close(to_chrome[1]);
            close(from_chrome[0]);
            close(from_chrome[1]);

            if (dup2(in_fd, 3) < 0 || dup2(out_fd, 4) < 0) {
                _exit(127);
            }
            close(in_fd);
            close(out_fd);

            fcntl(3, F_SETFD, 0);
            fcntl(4, F_SETFD, 0);

            int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (devnull >= 0) {
                dup2(devnull, 1);
                dup2(devnull, 2);
                close(devnull);
            }

            std::array<char, 512> url_buf{};
            size_t copy_len = initial_url.size() < 511 ? initial_url.size() : 511;
            std::memcpy(url_buf.data(), initial_url.data(), copy_len);
            url_buf[copy_len] = '\0';

            execl(chrome_path, "chromium", "--headless=new", "--remote-debugging-pipe",
                  "--no-sandbox", "--disable-gpu", "--disable-software-rasterizer",
                  "--disable-dev-shm-usage", "--user-data-dir=/tmp/axiom-chrome-profile",
                  url_buf.data(), nullptr);
            _exit(127);
        }

        close(to_chrome[0]);
        close(from_chrome[1]);

        to_chrome_fd_ = to_chrome[1];
        from_chrome_fd_ = from_chrome[0];
        child_pid_ = pid;
        started_ = true;

        usleep(500000);

        return attach_to_page();
    }

    /// Attaches CDP bridge to page target session.
    auto attach_to_page() noexcept -> Result<void> {
        int req_id = next_req_id_++;
        std::array<char, 128> cmd_buf{};
        int n = snprintf(cmd_buf.data(), cmd_buf.size(),
                         "{\"id\":%d,\"method\":\"Target.getTargets\"}", req_id);
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        auto write_res = write_pipe(std::string_view(cmd_buf.data(), static_cast<size_t>(n)));
        if (!write_res) {
            return std::unexpected(write_res.error());
        }

        auto read_res = read_msg_matching(req_id);
        if (!read_res) {
            return std::unexpected(read_res.error());
        }

        std::string_view resp = *read_res;
        size_t page_pos = resp.find("\"type\":\"page\"");
        if (page_pos == std::string_view::npos) {
            page_pos = resp.find("\"type\": \"page\"");
        }
        if (page_pos == std::string_view::npos) {
            return std::unexpected(SystemError::ProtocolError);
        }

        size_t tid_pos = resp.rfind("\"targetId\"", page_pos);
        if (tid_pos == std::string_view::npos) {
            tid_pos = resp.find("\"targetId\"", page_pos);
        }
        if (tid_pos == std::string_view::npos) {
            return std::unexpected(SystemError::ProtocolError);
        }

        std::string_view target_id =
            AXTreeFilter::extract_field_str(resp.substr(tid_pos), "\"targetId\"");
        if (target_id.empty()) {
            return std::unexpected(SystemError::ProtocolError);
        }

        int attach_id = next_req_id_++;
        std::array<char, 256> attach_cmd{};
        n = snprintf(attach_cmd.data(), attach_cmd.size(),
                     "{\"id\":%d,\"method\":\"Target.attachToTarget\",\"params\":{\"targetId\":\"%."
                     "*s\",\"flatten\":true}}",
                     attach_id, static_cast<int>(target_id.size()), target_id.data());
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        write_res = write_pipe(std::string_view(attach_cmd.data(), static_cast<size_t>(n)));
        if (!write_res) {
            return std::unexpected(write_res.error());
        }

        read_res = read_msg_matching(attach_id);
        if (!read_res) {
            return std::unexpected(read_res.error());
        }

        std::string_view session_str = AXTreeFilter::extract_field_str(*read_res, "\"sessionId\"");
        if (session_str.empty()) {
            return std::unexpected(SystemError::ProtocolError);
        }

        size_t copy_len = session_str.size() < session_id_.size() - 1 ? session_str.size()
                                                                      : session_id_.size() - 1;
        std::memcpy(session_id_.data(), session_str.data(), copy_len);
        session_id_[copy_len] = '\0';

        (void)send_session_cmd("Accessibility.enable", "{}");
        (void)send_session_cmd("Page.enable", "{}");
        (void)send_session_cmd("DOM.enable", "{}");

        return {};
    }

    /// Navigates the browser to the specified URL.
    auto navigate(std::string_view url) noexcept -> Result<void> {
        if (!started_) {
            auto s_res = start(url);
            if (!s_res) {
                return s_res;
            }
            return {};
        }

        std::array<char, 1024> params{};
        int n = snprintf(params.data(), params.size(), "{\"url\":\"%.*s\"}",
                         static_cast<int>(url.size()), url.data());
        if (n <= 0) {
            return std::unexpected(SystemError::InvalidArgument);
        }

        auto res = send_session_cmd("Page.navigate",
                                    std::string_view(params.data(), static_cast<size_t>(n)));
        if (!res) {
            return std::unexpected(res.error());
        }

        usleep(100000);
        return {};
    }

    /// Extracts and filters the semantic AXTree accessibility graph from Chromium.
    auto get_axtree(FixedVector<AXNode, MAX_AX_NODES>& out_nodes, std::span<char> out_text) noexcept
        -> Result<size_t> {
        if (!started_) {
            auto s_res = start();
            if (!s_res) {
                return std::unexpected(s_res.error());
            }
        }

        int req_id = next_req_id_++;
        std::array<char, 256> cmd{};
        int n =
            snprintf(cmd.data(), cmd.size(),
                     "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"Accessibility.getFullAXTree\"}",
                     req_id, session_id_.data());
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        auto write_res = write_pipe(std::string_view(cmd.data(), static_cast<size_t>(n)));
        if (!write_res) {
            return std::unexpected(write_res.error());
        }

        auto read_res = read_msg_matching(req_id);
        if (!read_res) {
            return std::unexpected(read_res.error());
        }

        return AXTreeFilter::filter_and_format(*read_res, out_nodes, out_text, handle_map_);
    }

    /// Dispatches a click interaction to the DOM element mapped to target handle.
    auto click(uint32_t handle) noexcept -> Result<void> {
        if (handle == 0 || handle >= handle_map_.size()) {
            return std::unexpected(SystemError::InvalidArgument);
        }
        int32_t backend_id = handle_map_[handle];
        if (backend_id == 0) {
            return std::unexpected(SystemError::InvalidArgument);
        }

        std::array<char, 128> resolve_params{};
        int n = snprintf(resolve_params.data(), resolve_params.size(), "{\"backendNodeId\":%d}",
                         backend_id);
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        int resolve_id = next_req_id_++;
        std::array<char, 256> resolve_cmd{};
        n = snprintf(
            resolve_cmd.data(), resolve_cmd.size(),
            "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"DOM.resolveNode\",\"params\":%.*s}",
            resolve_id, session_id_.data(), static_cast<int>(n), resolve_params.data());
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        auto write_res = write_pipe(std::string_view(resolve_cmd.data(), static_cast<size_t>(n)));
        if (!write_res) {
            return std::unexpected(write_res.error());
        }

        auto read_res = read_msg_matching(resolve_id);
        if (!read_res) {
            return std::unexpected(read_res.error());
        }

        std::string_view object_id = AXTreeFilter::extract_field_str(*read_res, "\"objectId\"");
        if (object_id.empty()) {
            return std::unexpected(SystemError::ProtocolError);
        }

        int call_id = next_req_id_++;
        std::array<char, 512> call_cmd{};
        n = snprintf(call_cmd.data(), call_cmd.size(),
                     "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"Runtime.callFunctionOn\","
                     "\"params\":{\"objectId\":\"%.*s\",\"functionDeclaration\":\"function() { "
                     "this.scrollIntoView({block: 'center'}); this.click(); }\"}}",
                     call_id, session_id_.data(), static_cast<int>(object_id.size()),
                     object_id.data());
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        write_res = write_pipe(std::string_view(call_cmd.data(), static_cast<size_t>(n)));
        if (!write_res) {
            return std::unexpected(write_res.error());
        }

        (void)read_msg_matching(call_id);
        return {};
    }

    /// Types text into the DOM element mapped to target handle.
    auto type_text(uint32_t handle, std::string_view text) noexcept -> Result<void> {
        if (handle == 0 || handle >= handle_map_.size()) {
            return std::unexpected(SystemError::InvalidArgument);
        }
        int32_t backend_id = handle_map_[handle];
        if (backend_id == 0) {
            return std::unexpected(SystemError::InvalidArgument);
        }

        std::array<char, 128> resolve_params{};
        int n = snprintf(resolve_params.data(), resolve_params.size(), "{\"backendNodeId\":%d}",
                         backend_id);
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        int resolve_id = next_req_id_++;
        std::array<char, 256> resolve_cmd{};
        n = snprintf(
            resolve_cmd.data(), resolve_cmd.size(),
            "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"DOM.resolveNode\",\"params\":%.*s}",
            resolve_id, session_id_.data(), static_cast<int>(n), resolve_params.data());
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        auto write_res = write_pipe(std::string_view(resolve_cmd.data(), static_cast<size_t>(n)));
        if (!write_res) {
            return std::unexpected(write_res.error());
        }

        auto read_res = read_msg_matching(resolve_id);
        if (!read_res) {
            return std::unexpected(read_res.error());
        }

        std::string_view object_id = AXTreeFilter::extract_field_str(*read_res, "\"objectId\"");
        if (object_id.empty()) {
            return std::unexpected(SystemError::ProtocolError);
        }

        int call_id = next_req_id_++;
        std::array<char, 1024> call_cmd{};
        n = snprintf(call_cmd.data(), call_cmd.size(),
                     "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"Runtime.callFunctionOn\","
                     "\"params\":{\"objectId\":\"%.*s\",\"functionDeclaration\":\"function(v) { "
                     "this.focus(); this.value = v; this.dispatchEvent(new Event('input', "
                     "{bubbles:true})); this.dispatchEvent(new Event('change', {bubbles:true})); "
                     "}\",\"arguments\":[{\"value\":\"%.*s\"}]}}",
                     call_id, session_id_.data(), static_cast<int>(object_id.size()),
                     object_id.data(), static_cast<int>(text.size()), text.data());
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        write_res = write_pipe(std::string_view(call_cmd.data(), static_cast<size_t>(n)));
        if (!write_res) {
            return std::unexpected(write_res.error());
        }

        (void)read_msg_matching(call_id);
        return {};
    }

    /// Scrolls the active viewport by the specified delta Y offset.
    auto scroll(int32_t delta_y) noexcept -> Result<void> {
        if (!started_) {
            return std::unexpected(SystemError::ProcessSpawnError);
        }

        int call_id = next_req_id_++;
        std::array<char, 256> cmd{};
        int n = snprintf(cmd.data(), cmd.size(),
                         "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"Runtime.evaluate\","
                         "\"params\":{\"expression\":\"window.scrollBy(0, %d)\"}}",
                         call_id, session_id_.data(), delta_y);
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        auto write_res = write_pipe(std::string_view(cmd.data(), static_cast<size_t>(n)));
        if (!write_res) {
            return std::unexpected(write_res.error());
        }

        (void)read_msg_matching(call_id);
        return {};
    }

    /// Gracefully terminates child Chromium process and closes pipe descriptors.
    void stop() noexcept {
        if (to_chrome_fd_ >= 0) {
            close(to_chrome_fd_);
            to_chrome_fd_ = -1;
        }
        if (from_chrome_fd_ >= 0) {
            close(from_chrome_fd_);
            from_chrome_fd_ = -1;
        }
        if (child_pid_ > 0) {
            kill(child_pid_, SIGKILL);
            waitpid(child_pid_, nullptr, 0);
            child_pid_ = -1;
        }
        started_ = false;
        session_id_[0] = '\0';
        next_req_id_ = 1;
        for (auto& h : handle_map_) {
            h = 0;
        }
    }

    /// Returns whether the CDP bridge is actively connected.
    [[nodiscard]] auto is_started() const noexcept -> bool { return started_; }

    /// Returns the backend DOM node ID mapped to a handle.
    [[nodiscard]] auto get_backend_node_id(uint32_t handle) const noexcept -> int32_t {
        if (handle < handle_map_.size()) {
            return handle_map_[handle];
        }
        return 0;
    }

    /// Directly sets a handle mapping for testing purposes.
    void set_handle_mapping(uint32_t handle, int32_t backend_id) noexcept {
        if (handle < handle_map_.size()) {
            handle_map_[handle] = backend_id;
        }
    }

private:
    int to_chrome_fd_{-1};
    int from_chrome_fd_{-1};
    pid_t child_pid_{-1};
    int next_req_id_{1};
    bool started_{false};
    std::array<char, 64> session_id_{};
    std::array<int32_t, MAX_HANDLES> handle_map_{};
    std::array<char, CDP_BUFFER_SIZE> rx_buf_{};

    /// Writes raw null-terminated command bytes to Chromium input pipe.
    auto write_pipe(std::string_view cmd) noexcept -> Result<void> {
        if (to_chrome_fd_ < 0) {
            return std::unexpected(SystemError::IoError);
        }

        size_t written = 0;
        while (written < cmd.size()) {
            ssize_t n = write(to_chrome_fd_, cmd.data() + written, cmd.size() - written);
            if (n <= 0) {
                return std::unexpected(SystemError::IoError);
            }
            written += static_cast<size_t>(n);
        }

        char null_byte = '\0';
        if (write(to_chrome_fd_, &null_byte, 1) != 1) {
            return std::unexpected(SystemError::IoError);
        }

        return {};
    }

    /// Sends a CDP command scoped to the attached page session.
    auto send_session_cmd(std::string_view method, std::string_view params) noexcept
        -> Result<int> {
        int req_id = next_req_id_++;
        std::array<char, 512> cmd{};
        int n = snprintf(cmd.data(), cmd.size(),
                         "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"%.*s\",\"params\":%.*s}",
                         req_id, session_id_.data(), static_cast<int>(method.size()), method.data(),
                         static_cast<int>(params.size()), params.data());
        if (n <= 0) {
            return std::unexpected(SystemError::ProtocolError);
        }

        auto write_res = write_pipe(std::string_view(cmd.data(), static_cast<size_t>(n)));
        if (!write_res) {
            return std::unexpected(write_res.error());
        }

        (void)read_msg_matching(req_id);
        return req_id;
    }

    /// Reads incoming null-delimited CDP messages until one matching req_id arrives or timeout
    /// expires.
    auto read_msg_matching(int req_id, int timeout_ms = 2000) noexcept -> Result<std::string_view> {
        if (from_chrome_fd_ < 0) {
            return std::unexpected(SystemError::IoError);
        }

        size_t rx_len = 0;
        std::array<char, 32> id_needle{};
        int n = snprintf(id_needle.data(), id_needle.size(), "\"id\":%d", req_id);
        std::string_view needle(id_needle.data(), n > 0 ? static_cast<size_t>(n) : 0);

        pollfd pfd{
            .fd = from_chrome_fd_,
            .events = POLLIN,
            .revents = 0,
        };

        auto start_time = std::chrono::steady_clock::now();

        while (true) {
            auto now = std::chrono::steady_clock::now();
            int elapsed = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count());
            int rem = timeout_ms - elapsed;
            if (rem <= 0) {
                return std::unexpected(SystemError::IoError);
            }

            int pr = poll(&pfd, 1, rem);
            if (pr <= 0) {
                return std::unexpected(SystemError::IoError);
            }

            char c = '\0';
            ssize_t rd = read(from_chrome_fd_, &c, 1);
            if (rd <= 0) {
                return std::unexpected(SystemError::IoError);
            }

            if (c == '\0') {
                std::string_view msg(rx_buf_.data(), rx_len);
                if (!needle.empty() && msg.find(needle) != std::string_view::npos) {
                    return msg;
                }
                rx_len = 0;
            } else {
                if (rx_len + 1 < rx_buf_.size()) {
                    rx_buf_[rx_len++] = c;
                }
            }
        }
    }
};

} // namespace axiom::cdp
