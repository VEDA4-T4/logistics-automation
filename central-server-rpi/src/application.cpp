#include "logistics/central_server/application.hpp"

#include <iostream>

#include "logistics/central_server/device_manager.hpp"

namespace logistics::central_server {

int Application::Run() {
    std::cout << "central-server scaffold: registered devices=" << DeviceManager::RegisteredDeviceCount() << '\n';
    return 0;
}

}  // namespace logistics::central_server
