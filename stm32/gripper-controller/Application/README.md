# Gripper controller application

The Gripper Raspberry Pi converts image coordinates into calibrated joint targets. The STM32 validates those
targets and controls four hobby servos: base, shoulder, elbow, and gripper.

## Runtime flow

```text
USART1 circular DMA
  -> uartRxQueue
  -> CommRxTask (parser and command validation)
  -> gripperControlQueue / safetyCommandQueue
  -> GripperControlTask / SafetyTask
  -> commTxQueue
  -> CommTxTask
  -> USART1
```

Motion commands are non-blocking. The control task interpolates targets every 20 ms and reports completion with an
asynchronous UART event. Completion is time-based because SG90/MG90S servos do not provide position feedback.

## Safety behavior

This design does not include a relay or a hardware-controlled servo power-cut output. On E-Stop the software stops
interpolation, invalidates the active motion, retains the last PWM value, and rejects new motion commands.
`RESET_DEVICE` releases the latch into `STOPPED`; it never starts motion or returns home automatically. The Raspberry
Pi must issue an explicit `HOME` command after confirming that the work area is safe.

Holding the last PWM reduces unintended movement or dropping, but it is not an independent safety-rated power
isolation mechanism.

## Calibration

Provisional joint limits, home angles, pulse widths, and gripper open/closed values are in
`Application/Inc/gripper_calibration.h`. Adjust them with the assembled arm before normal operation.
