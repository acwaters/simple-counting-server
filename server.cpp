#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Custom deleter that wraps a POSIX file descriptor

// We're going to be playing in POSIX land anyway, so there's no point in writing a bunch of useless
// abstraction around the sockets API when all we really want is to make sure it cleans itself up
// once we're finished with it. unique_ptr does a fine job of that for us; all we need is to give it
// a class that tells it how to store, compare, and release file descriptors.

struct fd_deleter {
    struct pointer {
        int fd;

        constexpr pointer(int fd) noexcept : fd(fd) {}
        constexpr pointer(decltype(nullptr) = nullptr) noexcept : fd(0) {}

        constexpr pointer(pointer&& other) noexcept  : fd(std::exchange(other.fd,0)) {}
        constexpr pointer(pointer const& other) noexcept : fd(other.fd) {}
        constexpr pointer& operator=(pointer&& other) noexcept { fd = std::exchange(other.fd,0); return *this; }
        constexpr pointer& operator=(pointer const& other) noexcept { fd = other.fd; return *this; }

        constexpr explicit operator bool() const { return fd; }

        constexpr auto operator<=>(pointer const&) const = default;
    };

    void operator()(pointer p) const noexcept {
        if (close(p.fd) == -1) {
            fprintf(stderr, "Warning: Failed to close file descriptor <%d>: ", p.fd);
            perror("");
        }
    }
};

using resource_handle = std::unique_ptr<void, fd_deleter>;  // file descriptors carry no type information, so we use void

// And now for the main event...
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
            auto connect_addr = sockaddr_in6{};
            auto connect_size = unsigned(sizeof(connect_addr));
            auto new_connection = resource_handle(accept(listen_socket.get().fd, reinterpret_cast<sockaddr*>(&connect_addr), &connect_size));
            if (new_connection.get().fd < 0) {
                perror("Failed to accept connection");
                new_connection.release();
                continue;
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

            // one of our connections hung up
            if (new_event.events & (EPOLLHUP | EPOLLRDHUP)) {
                std::erase_if(connections, [&](resource_handle const& handle) {
                    return handle.get().fd == new_event.data.fd;
                });

                fprintf(stderr, "%s hung up\n", peername[0] ? peername : "Peer");
            }

            if (new_event.events & EPOLLIN) {

            }
        }
    }
}
