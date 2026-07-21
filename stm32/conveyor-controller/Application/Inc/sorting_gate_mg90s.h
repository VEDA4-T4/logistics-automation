#ifndef SORTING_GATE_MG90S_H
#define SORTING_GATE_MG90S_H

#include "sorting_gate.h"

/* TIM3 is configured for 1 us/count and a 20 ms (50 Hz) period. */
#ifndef SORTING_GATE_HOME_PULSE_US
#define SORTING_GATE_HOME_PULSE_US 1500U
#endif

#ifndef SORTING_GATE_DESTINATION_1_PULSE_US
#define SORTING_GATE_DESTINATION_1_PULSE_US 1000U
#endif

#ifndef SORTING_GATE_DESTINATION_2_PULSE_US
#define SORTING_GATE_DESTINATION_2_PULSE_US 2000U
#endif

#ifndef SORTING_GATE_DESTINATION_3_PULSE_US
/* Destination 3 is the straight-through path and shares the Home position. */
#define SORTING_GATE_DESTINATION_3_PULSE_US SORTING_GATE_HOME_PULSE_US
#endif

#ifndef SORTING_GATE_SETTLE_TIME_MS
#define SORTING_GATE_SETTLE_TIME_MS 500U
#endif

const sorting_gate_port_t* sorting_gate_mg90s_port(void);

#endif /* SORTING_GATE_MG90S_H */
