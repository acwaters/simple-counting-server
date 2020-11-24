#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "posix-resource-handle.hpp"
#include "epoll-wrapper.hpp"

auto listen_on_dual_tcp_socket(uint16_t port) -> resource_handle;
auto accept_connection(int fd) -> resource_handle;
auto get_peer_name(int fd) -> std::string;
auto read_lines_from_fd(int fd) -> std::vector<std::string>;
void parse_and_handle(int fd, std::string command, std::vector<resource_handle>* connections, int64_t* count);

// This is global so that we don't have to
// capture it in our signal handler below
std::atomic<bool> running = true;

int main()
{
    struct sigaction handler;
    handler.sa_handler = [](int) { running = false; };

    if (sigaction(SIGINT,  &handler, nullptr) == -1 ||
        sigaction(SIGTERM, &handler, nullptr) == -1)
    {
        throw_system_error();
    }

    auto listen_socket = listen_on_dual_tcp_socket(8089);

    std::vector<resource_handle> connections;
    connections.reserve(1024);  // 4 KiB in exchange for zero reallocations on the first 1024 connections is a no-brainer

    auto poller = epoll();
    poller.add(listen_socket, EPOLLIN);

    fprintf(stderr, "Starting up... count initialized to 0\n");
    int64_t count = 0;

    while(running) {
        auto new_event = poller.wait();
        if (new_event.data.fd == 0) {
            // we were woken up by a signal
            continue;
        }

        // new incoming connection
        if (new_event.data.fd == listen_socket.get().fd) {
            auto new_connection = accept_connection(listen_socket.get().fd);
            if (new_connection) {
                poller.add(new_connection, EPOLLIN | EPOLLRDHUP);
                connections.push_back(std::move(new_connection));
            }
        }

        // event from one of our connections
        else {
            auto peer_name = get_peer_name(new_event.data.fd);

            // we have some data on one of our connections
            if (new_event.events & EPOLLIN) {
                for (auto const& line : read_lines_from_fd(new_event.data.fd)) {
                    parse_and_handle(new_event.data.fd, line, &connections, &count);
                }
            }

            // one of our connections hung up
            if (new_event.events & (EPOLLHUP | EPOLLRDHUP)) {
                std::erase_if(connections, [&](resource_handle const& handle) {
                    return handle.get().fd == new_event.data.fd;
                });

                fprintf(stderr, "%s hung up\n", peer_name.c_str());
            }
        }
    }

    fprintf(stderr, "Shutting down...\n");
}

auto listen_on_dual_tcp_socket(uint16_t port) -> resource_handle
{
    auto listen_socket = resource_handle(socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0));

    if (listen_socket.get().fd < 0) {
        listen_socket.release();
        throw_system_error();
    }

    // Default varies by platform, so explicitly opt into IPv4 connections on this socket
    uint32_t off = 0;
    if (setsockopt(listen_socket.get().fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
        throw_system_error();
    }

    auto listen_addr = sockaddr_in6 {
        .sin6_family = AF_INET6,
        .sin6_port   = htons(port),
        .sin6_addr   = IN6ADDR_ANY_INIT
    };

    if (bind(listen_socket.get().fd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)) < 0) {
        throw_system_error();
    }

    if (listen(listen_socket.get().fd, 64) < 0) {
        throw_system_error();
    }

    return listen_socket;
}

auto accept_connection(int listen_socket) -> resource_handle
{
    auto new_connection = resource_handle(accept4(listen_socket, nullptr, nullptr, SOCK_NONBLOCK));
    if (new_connection.get().fd < 0) {
        perror("Failed to accept connection");
        new_connection.release();
        return nullptr;
    }

    auto name = get_peer_name(new_connection.get().fd);
    fprintf(stderr, "New connection from %s\n", name.c_str());

    return new_connection;
}

auto get_peer_name(int conn_socket) -> std::string
{
    auto peer_addr = sockaddr_in6{};
    auto peer_size = unsigned(sizeof(peer_addr));
    if (getpeername(conn_socket, reinterpret_cast<sockaddr*>(&peer_addr), &peer_size) < 0) {
        perror("Failed to get peer address");
        return "peer";
    }

    if (peer_size != sizeof(peer_addr)) {
        fprintf(stderr, "Unexpected address size — peer is not IPv6 ???\n");
        return "peer";
    }

    static char buffer[1024];
    if (int err = getnameinfo(reinterpret_cast<sockaddr*>(&peer_addr), peer_size, buffer, sizeof(buffer), nullptr, 0, 0); err < 0) {
        fprintf(stderr, "Failed to get peer name: %s\n", gai_strerror(err));
        return "peer";
    }

    return buffer;
}

auto read_lines_from_fd(int infd) -> std::vector<std::string>
{
    auto in = fdopen(infd, "r");
    if (!in) {
        perror("Failed to open stream for file descriptor");
        return {};
    }

    std::vector<std::string> ret;

    char* buffer = nullptr;
    size_t length = 0;
    int bytes = 0;
    while ((bytes = getline(&buffer, &length, in)) > 0) {
        ret.push_back(std::string(buffer));
    }
    free(buffer);

    return ret;
}

void parse_and_handle(int fd, std::string command, std::vector<resource_handle>* connections, int64_t* count)
{
    auto send_count = [&](int fd) {
        auto output = std::to_string(*count);
        for (size_t sent = 0; sent < output.size(); ) {
            if (auto ret = send(fd, &output[sent], output.size(), MSG_NOSIGNAL); ret < 0) {
                if (errno == EINTR)
                    continue;

                fprintf(stderr, "Failed to send output on fd %d: ", fd);
                perror("");
                break;
            }

            else {
                sent += ret;
            }
        }
    };

    if (command == "OUTPUT\r\n") {
        fprintf(stderr, "%s requests the count; it is %ld\n", get_peer_name(fd).c_str(), *count);
        send_count(fd);
    }

    int64_t delta;
    if (sscanf(command.data(), "INCR %ld\r\n", &delta) == 1) {
        *count += delta;
        fprintf(stderr, "%s increments the count by %ld to %ld\n", get_peer_name(fd).c_str(), delta, *count);

        for (auto const& conn : *connections) {
            send_count(conn.get().fd);
        }
    }

    if (sscanf(command.data(), "DECR %ld\r\n", &delta) == 1) {
        *count -= delta;
        fprintf(stderr, "%s decrements the count by %ld to %ld\n", get_peer_name(fd).c_str(), delta, *count);

        for (auto const& conn : *connections) {
            send_count(conn.get().fd);
        }
    }
}
