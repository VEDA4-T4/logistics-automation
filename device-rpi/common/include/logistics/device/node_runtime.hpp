#pragma once

#include "logistics/contracts/device.hpp"

namespace logistics::device {

    class NodeRuntime final {
    public:
        explicit NodeRuntime(contracts::DeviceRole role);
        [[nodiscard]] int Run() const;

    private:
        contracts::DeviceRole role_;
    };

}  // namespace logistics::device
