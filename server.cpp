#include <cstdint>
#include <cstdio>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "posix-resource-handle.hpp"

void parse(char const* line) { fprintf(stderr, "%s", line); }

auto accept_connection(resource_handle const&) -> resource_handle;

int main() {
    auto listen_socket = resource_handle(socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0));

    if (listen_socket.get().fd < 0) {
        perror("Failed to open socket");
        listen_socket.release();
        return 1;
    }

    // Default varies by platform, so explicitly opt into IPv4 connections on this socket
    uint32_t off = 0;
    if (setsockopt(listen_socket.get().fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
        perror("Failed to unset IPv6-only");
        return 2;
    }

    auto listen_addr = sockaddr_in6 {
        .sin6_family = AF_INET6,
        .sin6_port   = htons(8089),
        .sin6_addr   = IN6ADDR_ANY_INIT
    };

    if (bind(listen_socket.get().fd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)) < 0) {
        perror("Failed to bind socket");
        return 3;
    }

    if (listen(listen_socket.get().fd, 64) < 0) {
        perror("Failed to listen on socket");
        return 4;
    }

    std::vector<resource_handle> connections;
    connections.reserve(1024);  // 4 KiB in exchange for zero reallocations on the first 1024 connections is a no-brainer

    auto epoll = resource_handle(epoll_create(1));
    if (epoll.get().fd < 0) {
        perror("Failed to create epoll");
        epoll.release();
        return 5;
    }

    auto event = epoll_event {
        .events = EPOLLIN,
        .data = { .fd = listen_socket.get().fd }
    };

    if (epoll_ctl(epoll.get().fd, EPOLL_CTL_ADD, listen_socket.get().fd, &event) < 0) {
        perror("Failed to add listening socket to epoll");
    }

    while(true) {
        auto new_event = epoll_event{};
        if (epoll_wait(epoll.get().fd, &new_event, 1, -1) < 0) {
            perror("Failed to wait on epoll");
            return 6;
        }

        // new incoming connection
        if (new_event.data.fd == listen_socket.get().fd) {
            auto new_connection = accept_connection(listen_socket);
            if (!new_connection) {
                continue;
            }

            auto data_event = epoll_event {
                .events = EPOLLIN | EPOLLRDHUP,
                .data = { .fd = new_connection.get().fd }
            };

            if (epoll_ctl(epoll.get().fd, EPOLL_CTL_ADD, new_connection.get().fd, &data_event) < 0) {
                perror("Failed to add new connection to epoll");
                continue;
            }

            connections.push_back(std::move(new_connection));
        }

        // event from one of our connections
        else {
            auto peer_addr = sockaddr_in6{};
            auto peer_size = unsigned(sizeof(peer_addr));
            static char peername[1024];
            if (getpeername(new_event.data.fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_size) == 0) {
                if (peer_size == sizeof(peer_addr)) {
                    if (getnameinfo(reinterpret_cast<sockaddr*>(&peer_addr), peer_size, peername, sizeof(peername), nullptr, 0, 0) < 0) {
                        perror("Failed to get peer name");
                        peername[0] = '\0';
                    }
                }
                else {
                    fprintf(stderr, "Warning: Unexpected peer address size\n");
                    peername[0] = '\0';
                }
            }
            else {
                perror("Failed to get peer address");
                peername[0] = '\0';
            }

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

                fprintf(stderr, "%s hung up\n", peername[0] ? peername : "Peer");
            }
        }
    }
}

auto accept_connection(resource_handle const& listen_socket) -> resource_handle
{
    auto connect_addr = sockaddr_in6{};
    auto connect_size = unsigned(sizeof(connect_addr));
    auto new_connection = resource_handle(accept4(listen_socket.get().fd, reinterpret_cast<sockaddr*>(&connect_addr), &connect_size, SOCK_NONBLOCK));
    if (new_connection.get().fd < 0) {
        perror("Failed to accept connection");
        new_connection.release();
        return nullptr;
    }

    if (connect_size == sizeof(connect_addr)) {
        static char peername[1024];
        if (getnameinfo(reinterpret_cast<sockaddr*>(&connect_addr), connect_size, peername, sizeof(peername), nullptr, 0, 0) == 0) {
            fprintf(stderr, "New connection from %s\n", peername);
        }
        else {
            perror("Failed to get connection name");
        }
    }
    else {
        fprintf(stderr, "Warning: Unexpected connection address size\n");
    }

    return new_connection;
}
