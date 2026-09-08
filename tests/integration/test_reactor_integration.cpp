#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "axiom/common.hpp"
#include "axiom/fixed_vector.hpp"
#include "axiom/wire.hpp"

/// Runs an end-to-end frame round-trip integration test across a socketpair using epoll.
auto main() -> int {
    int socket_pair[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, socket_pair) != 0) {
        std::perror("socketpair failed");
        return EXIT_FAILURE;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        std::perror("epoll_create1 failed");
        close(socket_pair[0]);
        close(socket_pair[1]);
        return EXIT_FAILURE;
    }

    epoll_event epoll_ev{};
    epoll_ev.events = EPOLLIN;
    epoll_ev.data.fd = socket_pair[1];
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_pair[1], &epoll_ev) != 0) {
        std::perror("epoll_ctl failed");
        close(epoll_fd);
        close(socket_pair[0]);
        close(socket_pair[1]);
        return EXIT_FAILURE;
    }

    axiom::wire::FrameHeader tx_header{
        .magic = axiom::wire::WIRE_MAGIC,
        .opcode = static_cast<uint16_t>(axiom::wire::Opcode::Exec),
        .request_id = 42,
        .payload_len = 4,
    };

    uint8_t tx_buf[axiom::wire::HEADER_SIZE + 4]{};
    auto encode_res = axiom::wire::encode_header(tx_header, tx_buf);
    if (!encode_res.has_value()) {
        std::fprintf(stderr, "encode_header failed\n");
        return EXIT_FAILURE;
    }
    tx_buf[axiom::wire::HEADER_SIZE] = 'E';
    tx_buf[axiom::wire::HEADER_SIZE + 1] = 'C';
    tx_buf[axiom::wire::HEADER_SIZE + 2] = 'H';
    tx_buf[axiom::wire::HEADER_SIZE + 3] = 'O';

    ssize_t bytes_written = write(socket_pair[0], tx_buf, sizeof(tx_buf));
    if (std::cmp_not_equal(bytes_written, sizeof(tx_buf))) {
        std::fprintf(stderr, "write incomplete: %zd\n", bytes_written);
        return EXIT_FAILURE;
    }

    epoll_event events[4]{};
    int nfds = epoll_wait(epoll_fd, events, 4, 1000);
    if (nfds <= 0) {
        std::fprintf(stderr, "epoll_wait timed out or failed\n");
        return EXIT_FAILURE;
    }

    axiom::FixedVector<uint8_t, 256> rx_ring;
    uint8_t read_buf[64]{};
    ssize_t bytes_read = read(events[0].data.fd, read_buf, sizeof(read_buf));
    if (std::cmp_not_equal(bytes_read, sizeof(tx_buf))) {
        std::fprintf(stderr, "read count mismatch: %zd\n", bytes_read);
        return EXIT_FAILURE;
    }

    for (ssize_t i = 0; i < bytes_read; ++i) {
        rx_ring.push_back(read_buf[static_cast<size_t>(i)]);
    }

    auto rx_header_res = axiom::wire::decode_header(rx_ring.as_span());
    if (!rx_header_res.has_value()) {
        std::fprintf(stderr, "decode_header failed\n");
        return EXIT_FAILURE;
    }

    const auto& rx_header = rx_header_res.value();
    if (rx_header.magic != axiom::wire::WIRE_MAGIC ||
        rx_header.opcode != static_cast<uint16_t>(axiom::wire::Opcode::Exec) ||
        rx_header.request_id != 42 || rx_header.payload_len != 4) {
        std::fprintf(stderr, "header field mismatch\n");
        return EXIT_FAILURE;
    }

    const uint8_t* payload_ptr = rx_ring.data() + axiom::wire::HEADER_SIZE;
    if (std::memcmp(payload_ptr, "ECHO", 4) != 0) {
        std::fprintf(stderr, "payload mismatch\n");
        return EXIT_FAILURE;
    }

    close(epoll_fd);
    close(socket_pair[0]);
    close(socket_pair[1]);

    std::printf("[integration-test] Reactor socketpair round-trip completed successfully\n");
    return EXIT_SUCCESS;
}
