#include <cstdint>
#include <cstdio>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main() {
    auto listenfd = socket(AF_INET6, SOCK_STREAM, 0);

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

    if (close(listenfd) < 0) {
        perror("failed to close socket");
        return 5;
    }
}
