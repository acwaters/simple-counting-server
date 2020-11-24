#include <cstdint>
#include <cstdio>
#include <memory>

#include <netinet/in.h>
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
    auto listen_socket = resource_handle(socket(AF_INET6, SOCK_STREAM, 0));

    auto listenfd = listen_socket.get().fd;

    if (listenfd < 0) {
        perror("failed to open socket");
        return 1;
    }

    // Default varies by platform, so explicitly opt into IPv4 connections on this socket
    uint32_t off = 0;
    if (setsockopt(listenfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
        perror("failed to unset IPv6-only");
        return 2;
    }

    auto listen_addr = sockaddr_in6 {
        .sin6_family = AF_INET6,
        .sin6_port   = htons(8089),
        .sin6_addr   = IN6ADDR_ANY_INIT
    };

    if (bind(listenfd, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)) < 0) {
        perror("failed to bind socket");
        return 3;
    }

    if (listen(listenfd, 64) < 0) {
        perror("failed to listen on socket");
        return 4;
    }
}
