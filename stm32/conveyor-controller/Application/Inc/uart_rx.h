#ifndef UART_RX_H
#define UART_RX_H

#include <stdint.h>

#include "app_messages.h"
#include "stm32f4xx_hal.h"

HAL_StatusTypeDef uart_rx_start(void);

HAL_StatusTypeDef uart_rx_restart(app_uart_channel_t channel);

uint32_t uart_rx_take_error(app_uart_channel_t channel);

uint32_t uart_rx_get_dropped_chunk_count(void);

#endif /* UART_RX_H */
