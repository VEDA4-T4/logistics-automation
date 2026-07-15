#include "logistics/control_center/process_control_state.hpp"

#include <cassert>

int main() {
    logistics::control_center::ProcessControlState state;
    assert(!state.isMqttConnected());
    assert(!state.isCommandPending());
    assert(!state.normalCommandsEnabled());
    assert(!state.emergencyStopEnabled());

    state.setMqttConnected(true);
    assert(state.normalCommandsEnabled());
    assert(state.emergencyStopEnabled());

    state.setCommandPending();
    assert(state.isCommandPending());
    assert(!state.normalCommandsEnabled());
    assert(state.emergencyStopEnabled());

    state.setCommandFinished();
    assert(!state.isCommandPending());
    assert(state.normalCommandsEnabled());

    state.setMqttConnected(false);
    assert(!state.isMqttConnected());
    assert(!state.isCommandPending());
    assert(!state.normalCommandsEnabled());
    assert(!state.emergencyStopEnabled());

    return 0;
}
