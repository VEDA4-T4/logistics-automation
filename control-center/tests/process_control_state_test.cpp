#include "logistics/control_center/process_control_state.hpp"

#include <cassert>

int main() {
    logistics::control_center::ProcessControlState state;
    assert(!state.isMqttConnected());
    assert(!state.isCommandPending());
    assert(!state.normalCommandsEnabled());
    assert(!state.startEnabled());
    assert(!state.stopEnabled());
    assert(!state.recoveryEnabled());
    assert(!state.emergencyStopEnabled());

    state.setMqttConnected(true);
    assert(state.normalCommandsEnabled());
    assert(!state.startEnabled());
    assert(!state.stopEnabled());
    assert(!state.recoveryEnabled());
    assert(state.emergencyStopEnabled());

    state.setPhase(logistics::control_center::ProcessControlPhase::EmergencyStop);
    assert(!state.startEnabled());
    assert(!state.stopEnabled());
    assert(state.recoveryEnabled());

    state.setCommandPending();
    assert(state.isCommandPending());
    assert(!state.normalCommandsEnabled());
    assert(!state.recoveryEnabled());
    assert(state.emergencyStopEnabled());

    state.setCommandFinished();
    assert(!state.isCommandPending());
    assert(state.normalCommandsEnabled());
    assert(state.recoveryEnabled());

    state.setPhase(logistics::control_center::ProcessControlPhase::Recovering);
    assert(state.recoveryEnabled());

    state.setPhase(logistics::control_center::ProcessControlPhase::Stopped);
    assert(state.startEnabled());
    assert(!state.stopEnabled());
    assert(!state.recoveryEnabled());

    state.setPhase(logistics::control_center::ProcessControlPhase::Running);
    assert(!state.startEnabled());
    assert(state.stopEnabled());

    state.setMqttConnected(false);
    assert(!state.isMqttConnected());
    assert(!state.isCommandPending());
    assert(!state.normalCommandsEnabled());
    assert(!state.startEnabled());
    assert(!state.stopEnabled());
    assert(!state.recoveryEnabled());
    assert(!state.emergencyStopEnabled());

    return 0;
}
