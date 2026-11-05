#ifndef SGAME_SIM_CLIENT_HPP
#define SGAME_SIM_CLIENT_HPP

#include <cerrno>
#include <cstring>
#include <mutex>
#include <type_traits>
#include "abi.h"

namespace sgame::simulation {

/* Attach after the author's Driver has completed its normal handshake.
 * All legacy calls and extension calls share that Driver's request_lock.
 * This class never loads a module, maps memory or opens a target fd. */
template<class LegacyDriver>
class Client {
public:
    using Request = typename LegacyDriver::request_obj;

    explicit Client(LegacyDriver &driver,
                    Request *shared = reinterpret_cast<Request *>(SGAME_SIM_SHARED_ADDRESS))
        : driver_(driver), shared_(shared) {}

    int capabilities(sgame_sim_caps &caps)
    {
        return exchange(SGAME_SIM_COMMAND_CAPS, &caps, sizeof(caps));
    }

#define SGAME_SIM_METHOD(command, name, type) \
    int name(type &req) { return exchange(command, &req, sizeof(req)); }
    SGAME_SIM_MEMORY_COMMANDS(SGAME_SIM_METHOD)
#undef SGAME_SIM_METHOD

    /* IDs must be positive, sorted and unique, as in enkernel. The kernel
     * snapshots all IDs before acquiring the target's one file reference. */
    int kgsl_query(dfm_kgsl_query_req &req, const uint32_t *excluded = nullptr)
    {
        if (req.excluded_ids || req.excluded_count > DFM_KGSL_QUERY_MAX_EXCLUDED_IDS ||
            (req.excluded_count && !excluded))
            return -EINVAL;
        return exchange(DFM_IOCTL_KGSL_QUERY, &req, sizeof(req),
                        excluded, req.excluded_count);
    }

private:
    int exchange(uint32_t command, void *payload, uint32_t bytes,
                 const uint32_t *excluded = nullptr, uint32_t count = 0)
    {
        static_assert(sizeof(shared_->vmemrw_info.user_buffer) >= sizeof(sgame_sim_packet));
        static_assert(sizeof(shared_->vmem_info) >= SGAME_SIM_EXTRA_BYTES);
        static_assert(sizeof(shared_->op) == sizeof(uint32_t));
        using Lock = std::remove_reference_t<decltype(driver_.request_lock)>;
        std::scoped_lock<Lock> lock(driver_.request_lock);
        const sgame_sim_header header { SGAME_SIM_ABI_VERSION, command, bytes, 0 };
        auto *buffer = reinterpret_cast<unsigned char *>(shared_->vmemrw_info.user_buffer);

        std::memcpy(buffer, &header, sizeof(header));
        std::memcpy(buffer + sizeof(header), payload, bytes);
        if (excluded)
            std::memcpy(&shared_->vmem_info, excluded, count * sizeof(*excluded));
        // An old module leaves an unknown operation's status untouched.
        __atomic_store_n(&shared_->status, -ENOTTY, __ATOMIC_RELAXED);
        const uint32_t operation = SGAME_SIM_REQUEST_OP;
        // Copy the representation: casting this extension value to the legacy
        // C++ enum would exceed that enum's specified value range.
        std::memcpy(const_cast<void *>(static_cast<const volatile void *>(&shared_->op)),
                    &operation, sizeof(operation));
        __atomic_store_n(&shared_->kernel, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&shared_->user, __ATOMIC_ACQUIRE)) {}
        const int status = __atomic_load_n(&shared_->status, __ATOMIC_RELAXED);
        std::memcpy(payload, buffer + sizeof(header), bytes);
        __atomic_store_n(&shared_->user, 0, __ATOMIC_RELAXED);
        return status;
    }

    LegacyDriver &driver_;
    Request *shared_;
};

} // namespace sgame::simulation
#endif
