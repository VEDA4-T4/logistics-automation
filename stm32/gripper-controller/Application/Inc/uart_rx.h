#ifndef UART_RX_H
#define UART_RX_H

#include <stdint.h>

#include "stm32f4xx_hal.h"

HAL_StatusTypeDef uart_rx_start(void);
HAL_StatusTypeDef uart_rx_restart(void);
uint32_t uart_rx_take_error(void);
uint32_t uart_rx_get_dropped_chunk_count(void);

#endif /* UART_RX_H */
