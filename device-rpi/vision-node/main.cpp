#include "logistics/device/node_runtime.hpp"

int main(int argc, char* argv[]) {
    return logistics::device::NodeRuntime{ logistics::contracts::DeviceRole::kVision }.Run(argc, argv);
}
