#pragma once
#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace Slic3r::NativePrintDispatch {

struct Mode {
    bool demo;
    bool provider;
};

// Read a fresh, synchronized optional<Mode> at both queue admission and dispatch.
// Losing the GUI snapshot never authorizes an untracked physical print.
template<class Snapshot, class Dispatch>
int run(Snapshot snapshot, Dispatch dispatch)
{
    try {
        const auto mode = snapshot();
        if (!mode || mode->demo || mode->provider)
            return -1;
    } catch (...) {
        return -1;
    }
    return dispatch();
}

// Shared leases preserve parallel native sends; reload must never wait on a GUI-thread callback.
template<class Agent, class Dispatch>
int with_agent(std::shared_mutex &lifetime, const std::atomic<Agent *> &slot, Dispatch dispatch)
{
    std::shared_lock<std::shared_mutex> lease(lifetime);
    auto *agent = slot.load();
    return agent ? dispatch(*agent) : -1;
}

template<class Agent>
bool try_detach(std::shared_mutex &lifetime, std::atomic<Agent *> &slot)
{
    std::unique_lock<std::shared_mutex> lease(lifetime, std::try_to_lock);
    if (!lease.owns_lock()) return false;
    slot.store(nullptr);
    return true;
}

} // namespace Slic3r::NativePrintDispatch
