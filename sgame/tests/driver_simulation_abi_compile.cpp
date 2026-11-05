/* Compile with the real, unchanged author header and Android NDK. */
#include "logger/logger.h"
#include "driver/driver.h"
#include "../../lsdriver/sgame_simulation/client.hpp"
#include <cstddef>

static_assert(sizeof(sgame_sim_header) == 16);
static_assert(sizeof(sgame_sim_caps) == 32);
static_assert(sizeof(sgame_sim_packet) == 1312);
static_assert(sizeof(mem_get_tls_req) == 16);
static_assert(sizeof(mem_get_thread_tids_req) == 1296);
static_assert(sizeof(dfm_kgsl_query_req) == 120);
static_assert(offsetof(dfm_kgsl_query_req, out_info) == 56);
static_assert(sizeof(Driver::request_obj::vmemrw_info.user_buffer) >= sizeof(sgame_sim_packet));
static_assert(sizeof(Driver::request_obj::vmem_info) >= SGAME_SIM_EXTRA_BYTES);
template class sgame::simulation::Client<Driver>;
