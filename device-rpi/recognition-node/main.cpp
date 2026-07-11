#include "logistics/device/node_runtime.hpp"

int main() {
    return logistics::device::NodeRuntime{logistics::contracts::DeviceRole::kRecognition}.Run();
}
