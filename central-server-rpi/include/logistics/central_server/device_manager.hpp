#pragma once

#include <cstddef>

namespace logistics::central_server {

class DeviceManager final {
public:
    [[nodiscard]] static std::size_t RegisteredDeviceCount() noexcept;
};

}  // namespace logistics::central_server
