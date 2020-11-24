#ifndef EPOLL_WRAPPER_HPP
#define EPOLL_WRAPPER_HPP

#include <sys/epoll.h>

#include "posix-resource-handle.hpp"

struct epoll {
    resource_handle handle;

    epoll() noexcept
      : handle(epoll_create(1))
    {
        if (handle.get().fd < 0) {
            throw_system_error();
        }
    }

    void add(resource_handle const& other, unsigned events)
    {
        auto event = epoll_event {
            .events = events,
            .data = { .fd = other.get().fd }
        };

        if (epoll_ctl(handle.get().fd, EPOLL_CTL_ADD, other.get().fd, &event) < 0) {
            throw_system_error();
        }
    }

    auto wait() -> epoll_event
    {
        auto event = epoll_event{};
        if (int err = epoll_wait(handle.get().fd, &event, 1, -1); err < 0) {
            if (errno == EINTR)
                return {};

            throw_system_error();
        }

        return event;
    }
};

#endif  // EPOLL_WRAPPER_HPP
