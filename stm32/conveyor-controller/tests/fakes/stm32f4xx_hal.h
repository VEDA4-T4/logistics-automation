#ifndef TEST_FAKES_STM32F4XX_HAL_H
#define TEST_FAKES_STM32F4XX_HAL_H

#include <stdint.h>

/* fakes/main.h와 동시에 include될 수 있어(hc_sr04.h 경유) 공유 guard로 중복 정의를 막는다. */
#ifndef TEST_FAKES_HAL_STATUS_TYPEDEF
#define TEST_FAKES_HAL_STATUS_TYPEDEF
typedef enum { HAL_OK = 0x00U, HAL_ERROR = 0x01U, HAL_BUSY = 0x02U } HAL_StatusTypeDef;
#endif

typedef struct __DMA_HandleTypeDef {
    uint32_t ErrorCode;
} DMA_HandleTypeDef;

typedef enum {
    HAL_UART_STATE_RESET = 0x00U,
    HAL_UART_STATE_READY = 0x20U,
    HAL_UART_STATE_BUSY = 0x24U,
    HAL_UART_STATE_BUSY_TX = 0x21U,
    HAL_UART_STATE_BUSY_RX = 0x22U,
    HAL_UART_STATE_BUSY_TX_RX = 0x23U
} HAL_UART_StateTypeDef;

typedef struct __UART_HandleTypeDef {
    uint32_t ErrorCode;
    DMA_HandleTypeDef* hdmatx;
    DMA_HandleTypeDef* hdmarx;
    HAL_UART_StateTypeDef gState;
    HAL_UART_StateTypeDef RxState;
} UART_HandleTypeDef;

#define HAL_UART_ERROR_NONE 0x00000000U
#define HAL_UART_ERROR_PE 0x00000001U
#define HAL_UART_ERROR_NE 0x00000002U
#define HAL_UART_ERROR_FE 0x00000004U
#define HAL_UART_ERROR_ORE 0x00000008U
#define HAL_UART_ERROR_DMA 0x00000010U
#define HAL_DMA_ERROR_NONE 0x00000000U
#define HAL_DMA_ERROR_TE 0x00000001U

/* 호스트 빌드에는 CMSIS 코어 인트린식이 없어 no-op으로 대체한다. */
static inline uint32_t __get_PRIMASK(void) {
    return 0U;
}

static inline void __disable_irq(void) {}

static inline void __enable_irq(void) {}

/* 폴트 상태 레지스터. 테스트가 값을 직접 넣을 수 있도록 변수로 둔다. */
typedef struct {
    uint32_t CFSR;
    uint32_t HFSR;
    uint32_t BFAR;
    uint32_t MMFAR;
} SCB_FakeTypeDef;

extern SCB_FakeTypeDef fakeScb;
#define SCB (&fakeScb)

/* 스택 포인터도 테스트가 지정한다. */
extern uint32_t fakeMsp;
extern uint32_t fakePsp;

static inline uint32_t __get_MSP(void) {
    return fakeMsp;
}

static inline uint32_t __get_PSP(void) {
    return fakePsp;
}

uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef* huart, const uint8_t* data, uint16_t length);
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef* huart);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef* huart);
HAL_StatusTypeDef HAL_UART_DMAStop(UART_HandleTypeDef* huart);
HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef* huart, uint8_t* data, uint16_t size);

void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart);
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t size);

#endif /* TEST_FAKES_STM32F4XX_HAL_H */
