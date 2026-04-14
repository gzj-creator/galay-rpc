#ifndef GALAY_RPC_RUNTIME_COMPAT_H
#define GALAY_RPC_RUNTIME_COMPAT_H

#include <algorithm>
#include <cstddef>
#include <thread>

namespace galay::rpc
{

inline size_t resolveIoSchedulerCount(size_t requested)
{
    if (requested != 0) {
        return requested;
    }

    const auto detected = static_cast<size_t>(std::thread::hardware_concurrency());
    return std::max<size_t>(1, detected);
}

} // namespace galay::rpc

#endif // GALAY_RPC_RUNTIME_COMPAT_H
