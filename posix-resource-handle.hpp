#ifndef POSIX_RESOURCE_WRAPPER_HPP
#define POSIX_RESOURCE_WRAPPER_HPP

#include <memory>
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
        constexpr pointer(decltype(nullptr) = nullptr) noexcept : fd(-1) {}

        constexpr pointer(pointer&& other) noexcept  : fd(std::exchange(other.fd,-1)) {}
        constexpr pointer(pointer const& other) noexcept : fd(other.fd) {}
        constexpr pointer& operator=(pointer&& other) noexcept { fd = std::exchange(other.fd,-1); return *this; }
        constexpr pointer& operator=(pointer const& other) noexcept { fd = other.fd; return *this; }

        constexpr explicit operator bool() const { return fd != -1; }

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

// A small utility function for transforming fatal errors into exceptions
[[noreturn]]
void throw_system_error()
{
    throw std::system_error(std::make_error_code(std::errc(errno)));
}

#endif  // POSIX_RESOURCE_WRAPPER_HPP
