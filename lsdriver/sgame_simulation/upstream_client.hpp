#ifndef SGAME_SIM_UPSTREAM_CLIENT_HPP
#define SGAME_SIM_UPSTREAM_CLIENT_HPP

#include "logger/logger.h"
#include "driver/driver.h"
#include "client.hpp"

namespace sgame::simulation::detail {

struct UpstreamRequestLock {
    using Type = std::mutex Driver::*;
    friend Type request_lock_member(UpstreamRequestLock);
};

template struct RequestLockMember<UpstreamRequestLock, &::Driver::request_lock>;

template<>
struct RequestLock<::Driver> {
    static std::mutex &get(::Driver &driver)
    {
        return driver.*request_lock_member(UpstreamRequestLock{});
    }
};

} // namespace sgame::simulation::detail
#endif
