#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace logistics::vision {

class VisionStateTransaction final {
public:
    class LockedState final {
    public:
        void ClearWork() noexcept {
            ++generation_;
        }

        [[nodiscard]] std::uint64_t Generation() const noexcept {
            return generation_;
        }

    private:
        friend class VisionStateTransaction;
        explicit LockedState(std::uint64_t& generation) : generation_(generation) {}

        std::uint64_t& generation_;
    };

    template <typename Operation>
    decltype(auto) Synchronize(Operation&& operation) {
        std::lock_guard lock(mutex_);
        LockedState state(generation_);
        return std::invoke(std::forward<Operation>(operation), state);
    }

    [[nodiscard]] std::uint64_t CaptureGeneration() const {
        std::lock_guard lock(mutex_);
        return generation_;
    }

    template <typename Operational, typename Publication>
    [[nodiscard]] bool PublishIfCurrent(const std::uint64_t generation, Operational&& operational,
                                        Publication&& publication) {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || !std::invoke(std::forward<Operational>(operational))) {
            return false;
        }
        std::invoke(std::forward<Publication>(publication));
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::uint64_t generation_{};
};

}  // namespace logistics::vision
