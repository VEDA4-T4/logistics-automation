#include "logistics/device/node_runtime.hpp"

#include <iostream>

namespace logistics::device {

NodeRuntime::NodeRuntime(contracts::DeviceRole role) : role_(role) {}

int NodeRuntime::Run() const {
    std::cout << "device node scaffold: role=" << contracts::ToString(role_) << '\n';
    return 0;
}

}  // namespace logistics::device
