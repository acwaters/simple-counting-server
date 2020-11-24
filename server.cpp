#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "posix-resource-handle.hpp"
#include "epoll-wrapper.hpp"

void parse(char const* line) { fprintf(stderr, "%s", line); }

auto listen_on_dual_tcp_socket(uint16_t port) -> resource_handle;
auto accept_connection(int) -> resource_handle;
auto get_peer_name(int) -> std::string;

int main()
{
    auto listen_socket = listen_on_dual_tcp_socket(8089);

    std::vector<resource_handle> connections;
    connections.reserve(1024);  // 4 KiB in exchange for zero reallocations on the first 1024 connections is a no-brainer

    auto poller = epoll();
    poller.add(listen_socket, EPOLLIN);

    while(true) {
        auto new_event = poller.wait();

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
                auto in = fdopen(new_event.data.fd, "r");
                if (!in) {
                    perror("Failed to open stream for socket");
                    continue;
                }

                char* buffer = nullptr;
                size_t length = 0;
                int bytes = 0;
                while ((bytes = getline(&buffer, &length, in)) > 0) {
                    parse(buffer);
                }

                free(buffer);
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
    if (getnameinfo(reinterpret_cast<sockaddr*>(&peer_addr), peer_size, buffer, sizeof(buffer), nullptr, 0, 0) < 0) {
        perror("Failed to get peer name");
        return "peer";
    }

    return buffer;
}
