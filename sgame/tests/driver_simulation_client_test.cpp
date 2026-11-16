#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include "../../lsdriver/sgame_simulation/client.hpp"
extern "C" {
#include "../../lsdriver/sgame_simulation/internal.h"
void sgame_fixture_reset(void);
void sgame_fixture_memory_error(int error);
}

class FixtureDriver {
public:
    enum request_op { none, read };
    struct request_obj {
        volatile int kernel = 0, user = 0;
        volatile request_op op = none;
        volatile int status = 0;
        struct { unsigned char user_buffer[4096]; } vmemrw_info {};
        unsigned char vmem_info[SGAME_SIM_EXTRA_BYTES] {};
    } shared;
    int legacy_read()
    {
        std::scoped_lock lock(request_lock);
        shared.op = read;
        __atomic_store_n(&shared.kernel, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&shared.user, __ATOMIC_ACQUIRE)) {}
        int status = __atomic_load_n(&shared.status, __ATOMIC_RELAXED);
        __atomic_store_n(&shared.user, 0, __ATOMIC_RELAXED);
        return status;
    }
private:
    std::mutex request_lock;
};

namespace sgame::simulation::detail {
struct FixtureRequestLock {
    using Type = std::mutex FixtureDriver::*;
    friend Type request_lock_member(FixtureRequestLock);
};
template struct RequestLockMember<FixtureRequestLock, &FixtureDriver::request_lock>;
template<>
struct RequestLock<FixtureDriver> {
    static std::mutex &get(FixtureDriver &driver)
    {
        return driver.*request_lock_member(FixtureRequestLock{});
    }
};
} // namespace sgame::simulation::detail

class DispatchWorker {
public:
    explicit DispatchWorker(FixtureDriver &driver, bool old_module = false)
        : worker_([this, &driver, old_module] {
            auto &r = driver.shared;
            while (!stop_.load()) {
                if (!__atomic_load_n(&r.kernel, __ATOMIC_ACQUIRE)) {
                    std::this_thread::yield();
                    continue;
                }
                __atomic_store_n(&r.kernel, 0, __ATOMIC_RELAXED);
                uint32_t op;
                std::memcpy(&op, const_cast<const void *>(
                    static_cast<const volatile void *>(&r.op)), sizeof(op));
                if (op == FixtureDriver::read) {
                    __atomic_store_n(&r.status, 0x5a, __ATOMIC_RELAXED);
                } else if (op == SGAME_SIM_REQUEST_OP && !old_module) {
                    int status = sgame_sim_dispatch(r.vmemrw_info.user_buffer,
                        sizeof(r.vmemrw_info.user_buffer), r.vmem_info, sizeof(r.vmem_info));
                    __atomic_store_n(&r.status, status, __ATOMIC_RELAXED);
                }
                __atomic_store_n(&r.user, 1, __ATOMIC_RELEASE);
            }
        }) {}
    ~DispatchWorker() { stop_.store(true); worker_.join(); }
private:
    std::atomic<bool> stop_ {false};
    std::thread worker_;
};

template class sgame::simulation::Client<FixtureDriver>;

int main()
{
    sgame_fixture_reset();
    {
        FixtureDriver driver;
        DispatchWorker worker(driver);
        sgame::simulation::Client client(driver, &driver.shared);
        sgame_sim_caps caps {};
        assert(!client.capabilities(caps));
        assert(caps.version_code == SGAME_SIM_VERSION_CODE && caps.packet_bytes == 1312);
        // Concurrent calls through the legacy API and the extension must serialize.
        std::thread reader([&] {
            for (int i = 0; i < 128; ++i) assert(driver.legacy_read() == 0x5a);
        });
        std::thread environment([&] {
            for (int i = 0; i < 128; ++i) {
                mem_get_tls_req tls {};
                tls.pid = 77;
                assert(!client.get_tls(tls) && tls.out_tls == 7700);
            }
        });
        reader.join();
        environment.join();
        dfm_kgsl_query_req query {};
        query.abi_version = DFM_KGSL_QUERY_ABI_VERSION;
        query.pid = 77;
        query.target_fd = 3;
        query.op = DFM_KGSL_QUERY_SYNCSOURCE_PEEK;
        query.min_id = 1;
        query.excluded_count = 3;
        const uint32_t excluded[] {1, 2, 3};
        assert(!client.kgsl_query(query, excluded) && query.out_id == 4);
        query.excluded_ids = 0x1234;
        assert(client.kgsl_query(query, excluded) == -EINVAL);
    }
    sgame_fixture_reset();
    {
        FixtureDriver driver;
        DispatchWorker old_worker(driver,true);
        sgame::simulation::Client client(driver,&driver.shared);
        sgame_sim_caps caps{};
        assert(client.capabilities(caps)==-ENOTTY);
    }
    std::puts("simulation client: legacy concurrency, KGSL transport and old-module detection passed");
}
