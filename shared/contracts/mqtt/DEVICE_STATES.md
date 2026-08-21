# Device state semantics

`shared/include/logistics/contracts/device.hpp` is the single source for the role-aware `currentState` vocabulary and
its semantic categories: idle, working, stopped, error, emergency stop, recovery, and completed. Producers send the
canonical uppercase values defined there. Consumers derive `DeviceRole` from configured device identity and call
`DeviceStateMeaningFor` instead of maintaining another state list.

MQTT protocol 1.0 continues to accept any non-empty `currentState` for forward compatibility. A value outside the
role's vocabulary maps to `kUnknown`: it may be preserved for display and logging, but must not advance process
stages, enable motion, or by itself raise a system failure.

Line-tracer `currentState` describes the controller's operational phase. The optional `movementState` in the same
status describes position telemetry only (`IDLE`, `MOVING`, or `ARRIVED`); it does not replace `currentState` and must
not independently advance the central process.
