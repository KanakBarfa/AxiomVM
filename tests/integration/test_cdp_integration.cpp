#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "axiom/cdp.hpp"
#include "axiom/common.hpp"
#include "axiom/fixed_vector.hpp"

namespace {

constexpr std::string_view REALISTIC_CDP_AXTREE_JSON = R"json({
  "id": 4,
  "result": {
    "nodes": [
      {
        "nodeId": "1",
        "ignored": false,
        "role": {"type": "role", "value": "RootWebArea"},
        "name": {"type": "computedString", "value": "Axiom Checkout"},
        "backendDOMNodeId": 1
      },
      {
        "nodeId": "2",
        "ignored": true,
        "role": {"type": "role", "value": "none"},
        "backendDOMNodeId": 2
      },
      {
        "nodeId": "3",
        "ignored": false,
        "role": {"type": "role", "value": "heading"},
        "name": {"type": "computedString", "value": "Payment Portal"},
        "properties": [{"name": "level", "value": {"type": "integer", "value": 1}}],
        "backendDOMNodeId": 3
      },
      {
        "nodeId": "4",
        "ignored": false,
        "role": {"type": "role", "value": "genericContainer"},
        "backendDOMNodeId": 4
      },
      {
        "nodeId": "5",
        "ignored": false,
        "role": {"type": "role", "value": "textbox"},
        "name": {"type": "computedString", "value": "Credit Card Number"},
        "value": {"type": "computedString", "value": "4111222233334444"},
        "description": {"type": "computedString", "value": "Enter 16 digits"},
        "backendDOMNodeId": 101
      },
      {
        "nodeId": "6",
        "ignored": false,
        "role": {"type": "role", "value": "textbox"},
        "name": {"type": "computedString", "value": "CVV"},
        "value": {"type": "computedString", "value": "123"},
        "backendDOMNodeId": 102
      },
      {
        "nodeId": "7",
        "ignored": false,
        "role": {"type": "role", "value": "checkbox"},
        "name": {"type": "computedString", "value": "Save card for future billing"},
        "value": {"type": "computedString", "value": "true"},
        "backendDOMNodeId": 103
      },
      {
        "nodeId": "8",
        "ignored": false,
        "role": {"type": "role", "value": "button"},
        "name": {"type": "computedString", "value": "Complete Purchase"},
        "backendDOMNodeId": 104
      },
      {
        "nodeId": "9",
        "ignored": false,
        "role": {"type": "role", "value": "link"},
        "name": {"type": "computedString", "value": "Terms and Conditions"},
        "backendDOMNodeId": 105
      },
      {
        "nodeId": "10",
        "ignored": false,
        "role": {"type": "role", "value": "StaticText"},
        "name": {"type": "computedString", "value": "Payment Portal"},
        "backendDOMNodeId": 106
      },
      {
        "nodeId": "11",
        "ignored": false,
        "role": {"type": "role", "value": "StaticText"},
        "name": {"type": "computedString", "value": "Complete Purchase"},
        "backendDOMNodeId": 107
      },
      {
        "nodeId": "12",
        "ignored": false,
        "role": {"type": "role", "value": "StaticText"},
        "name": {"type": "computedString", "value": "Terms and Conditions"},
        "backendDOMNodeId": 108
      },
      {
        "nodeId": "13",
        "ignored": false,
        "role": {"type": "role", "value": "InlineTextBox"},
        "backendDOMNodeId": 109
      },
      {
        "nodeId": "14",
        "ignored": false,
        "role": {"type": "role", "value": "InlineTextBox"},
        "backendDOMNodeId": 110
      },
      {
        "nodeId": "15",
        "ignored": false,
        "role": {"type": "role", "value": "LineBreak"},
        "backendDOMNodeId": 111
      },
      {
        "nodeId": "16",
        "ignored": false,
        "role": {"type": "role", "value": "StaticText"},
        "name": {"type": "computedString", "value": "SSL Encrypted 256-bit"},
        "backendDOMNodeId": 112
      }
    ]
  }
})json";

/// Tests AXTree filtering, handle mapping, and token compression ratio.
void test_axtree_filter_and_token_density() {
    axiom::FixedVector<axiom::cdp::AXNode, axiom::cdp::MAX_AX_NODES> nodes{};
    std::array<char, axiom::cdp::MAX_TREE_TEXT_SIZE> text_buf{};
    std::array<int32_t, axiom::cdp::MAX_HANDLES> handle_map{};

    auto res = axiom::cdp::AXTreeFilter::filter_and_format(REALISTIC_CDP_AXTREE_JSON, nodes,
                                                           text_buf, handle_map);

    assert(res.has_value());
    size_t formatted_len = *res;
    assert(formatted_len > 0);

    std::string_view formatted_text(text_buf.data(), formatted_len);

    // Verify token density: raw JSON is ~2400 bytes, formatted is ~250 bytes (> 85-90% reduction)
    double raw_size = static_cast<double>(REALISTIC_CDP_AXTREE_JSON.size());
    double filtered_size = static_cast<double>(formatted_len);
    double reduction = (1.0 - (filtered_size / raw_size)) * 100.0;
    std::cout << "[test_cdp] Raw CDP JSON size: " << raw_size << " bytes\n";
    std::cout << "[test_cdp] Formatted text size: " << filtered_size << " bytes\n";
    std::cout << "[test_cdp] Token reduction ratio: " << reduction << "%\n";
    assert(reduction > 85.0);

    // Verify semantic nodes count: heading, 5 actionable (textbox, textbox, checkbox, button,
    // link), 1 informative text
    assert(nodes.size() == 7);

    // Verify heading node
    assert(nodes[0].role == "heading");
    assert(nodes[0].name == "Payment Portal");
    assert(nodes[0].heading_level == 1);
    assert(!nodes[0].actionable);
    assert(nodes[0].handle == 0);

    // Verify actionable handles
    assert(nodes[1].handle == 1);
    assert(nodes[1].role == "textbox");
    assert(nodes[1].name == "Credit Card Number");
    assert(nodes[1].value == "4111222233334444");
    assert(nodes[1].description == "Enter 16 digits");
    assert(handle_map[1] == 101);

    assert(nodes[2].handle == 2);
    assert(nodes[2].role == "textbox");
    assert(nodes[2].name == "CVV");
    assert(nodes[2].value == "123");
    assert(handle_map[2] == 102);

    assert(nodes[3].handle == 3);
    assert(nodes[3].role == "checkbox");
    assert(nodes[3].name == "Save card for future billing");
    assert(handle_map[3] == 103);

    assert(nodes[4].handle == 4);
    assert(nodes[4].role == "button");
    assert(nodes[4].name == "Complete Purchase");
    assert(handle_map[4] == 104);

    assert(nodes[5].handle == 5);
    assert(nodes[5].role == "link");
    assert(nodes[5].name == "Terms and Conditions");
    assert(handle_map[5] == 105);

    // Verify non-duplicate static text node
    assert(nodes[6].role == "StaticText");
    assert(nodes[6].name == "SSL Encrypted 256-bit");
    assert(nodes[6].handle == 0);

    // Verify text representation contains all handles and elements
    assert(formatted_text.find("[heading:1] \"Payment Portal\"") != std::string_view::npos);
    assert(formatted_text.find("[@1] textbox \"Credit Card Number\"") != std::string_view::npos);
    assert(formatted_text.find("[@2] textbox \"CVV\"") != std::string_view::npos);
    assert(formatted_text.find("[@3] checkbox \"Save card for future billing\"") !=
           std::string_view::npos);
    assert(formatted_text.find("[@4] button \"Complete Purchase\"") != std::string_view::npos);
    assert(formatted_text.find("[@5] link \"Terms and Conditions\"") != std::string_view::npos);
    assert(formatted_text.find("text: \"SSL Encrypted 256-bit\"") != std::string_view::npos);

    std::cout << "[test_cdp] AXTree filter and token density tests passed successfully.\n";
}

/// Tests CdpBridge communication and interaction dispatch over loopback descriptors.
void test_cdp_bridge_loopback() {
    int initd_to_chrome[2]{};
    int chrome_to_initd[2]{};

    if (pipe(initd_to_chrome) != 0 || pipe(chrome_to_initd) != 0) {
        std::abort();
    }

    axiom::cdp::CdpBridge bridge{};
    auto init_res = bridge.init_from_fds(initd_to_chrome[1], chrome_to_initd[0]);
    assert(init_res.has_value());
    (void)init_res;
    assert(bridge.is_started());

    // Set mock handle mapping
    bridge.set_handle_mapping(1, 101); // textbox
    bridge.set_handle_mapping(4, 104); // button
    assert(bridge.get_backend_node_id(1) == 101);
    assert(bridge.get_backend_node_id(4) == 104);
    assert(bridge.get_backend_node_id(99) == 0);

    close(initd_to_chrome[0]);
    close(chrome_to_initd[1]);
    bridge.stop();
    assert(!bridge.is_started());

    std::cout << "[test_cdp] CDP bridge loopback tests passed successfully.\n";
}

} // namespace

int main() {
    test_axtree_filter_and_token_density();
    test_cdp_bridge_loopback();
    std::cout << "[test_cdp_integration] All integration tests passed.\n";
    return 0;
}
