#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "uart_rx.h"

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart6;

static DMA_HandleTypeDef fakeDmaTx1;
static DMA_HandleTypeDef fakeDmaRx1;
static DMA_HandleTypeDef fakeDmaTx6;
static DMA_HandleTypeDef fakeDmaRx6;

static uint32_t dmaStopCalls;
static uint32_t abortReceiveCalls;
static uint32_t receiveToIdleCalls;
static UART_HandleTypeDef* lastReceiveToIdleHandle;
static HAL_StatusTypeDef receiveToIdleResult;
static uint32_t commTxErrorCalls;

static uint8_t queueToken;
osMessageQueueId_t uartRxQueueHandle = &queueToken;

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void* msg_ptr, uint8_t msg_prio, uint32_t timeout) {
    (void)mq_id;
    (void)msg_ptr;
    (void)msg_prio;
    (void)timeout;
    return osOK;
}

HAL_StatusTypeDef HAL_UART_DMAStop(UART_HandleTypeDef* huart) {
    dmaStopCalls++;

    /* 실제 HAL과 같이 송신과 수신을 함께 내린다. */
    if (huart->gState == HAL_UART_STATE_BUSY_TX) {
        huart->gState = HAL_UART_STATE_READY;
    }

    huart->RxState = HAL_UART_STATE_READY;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef* huart) {
    abortReceiveCalls++;
    huart->RxState = HAL_UART_STATE_READY;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef* huart, uint8_t* data, uint16_t size) {
    (void)data;
    (void)size;

    receiveToIdleCalls++;
    lastReceiveToIdleHandle = huart;

    if (receiveToIdleResult != HAL_OK) {
        return receiveToIdleResult;
    }

    huart->RxState = HAL_UART_STATE_BUSY_RX;
    return HAL_OK;
}

void CommTx_HandleUartError(UART_HandleTypeDef* huart) {
    (void)huart;
    commTxErrorCalls++;
}

static void reset_all(void) {
    memset(&huart1, 0, sizeof(huart1));
    memset(&huart6, 0, sizeof(huart6));
    memset(&fakeDmaTx1, 0, sizeof(fakeDmaTx1));
    memset(&fakeDmaRx1, 0, sizeof(fakeDmaRx1));
    memset(&fakeDmaTx6, 0, sizeof(fakeDmaTx6));
    memset(&fakeDmaRx6, 0, sizeof(fakeDmaRx6));

    huart1.hdmatx = &fakeDmaTx1;
    huart1.hdmarx = &fakeDmaRx1;
    huart1.gState = HAL_UART_STATE_READY;
    huart1.RxState = HAL_UART_STATE_BUSY_RX;

    huart6.hdmatx = &fakeDmaTx6;
    huart6.hdmarx = &fakeDmaRx6;
    huart6.gState = HAL_UART_STATE_READY;
    huart6.RxState = HAL_UART_STATE_BUSY_RX;

    dmaStopCalls = 0U;
    abortReceiveCalls = 0U;
    receiveToIdleCalls = 0U;
    lastReceiveToIdleHandle = (UART_HandleTypeDef*)0;
    receiveToIdleResult = HAL_OK;
    commTxErrorCalls = 0U;

    (void)uart_rx_take_error(APP_UART_CHANNEL_1);
    (void)uart_rx_take_error(APP_UART_CHANNEL_6);
}

/*
 * 이 테스트가 이번 수정의 핵심이다.
 * 예전 구현은 HAL_UART_DMAStop()을 써서 수신을 복구할 때마다
 * 전송 중이던 heartbeat/response 프레임까지 잘라먹었다.
 */
static void test_restart_keeps_in_flight_transmit_alive(void) {
    reset_all();
    huart1.gState = HAL_UART_STATE_BUSY_TX;

    assert(uart_rx_restart(APP_UART_CHANNEL_1) == HAL_OK);

    assert(dmaStopCalls == 0U);
    assert(abortReceiveCalls == 1U);
    assert(huart1.gState == HAL_UART_STATE_BUSY_TX);
    assert(receiveToIdleCalls == 1U);
    assert(lastReceiveToIdleHandle == &huart1);
    assert(huart1.RxState == HAL_UART_STATE_BUSY_RX);
}

static void test_restart_targets_only_the_requested_channel(void) {
    reset_all();
    huart6.gState = HAL_UART_STATE_BUSY_TX;

    assert(uart_rx_restart(APP_UART_CHANNEL_6) == HAL_OK);

    assert(lastReceiveToIdleHandle == &huart6);
    assert(huart6.gState == HAL_UART_STATE_BUSY_TX);
    assert(huart1.RxState == HAL_UART_STATE_BUSY_RX);
    assert(receiveToIdleCalls == 1U);
}

static void test_restart_rejects_unknown_channel(void) {
    reset_all();

    assert(uart_rx_restart((app_uart_channel_t)3) == HAL_ERROR);
    assert(abortReceiveCalls == 0U);
    assert(receiveToIdleCalls == 0U);
}

static void test_restart_reports_failed_receive_start(void) {
    reset_all();
    receiveToIdleResult = HAL_BUSY;

    assert(uart_rx_restart(APP_UART_CHANNEL_1) == HAL_BUSY);
    assert(abortReceiveCalls == 1U);
}

static void test_receive_error_requests_recovery(void) {
    reset_all();

    /* 오버런은 HAL이 수신을 내린 뒤 콜백을 부른다. */
    huart1.ErrorCode = HAL_UART_ERROR_ORE;
    huart1.RxState = HAL_UART_STATE_READY;
    HAL_UART_ErrorCallback(&huart1);

    assert(uart_rx_take_error(APP_UART_CHANNEL_1) != 0U);
    assert(uart_rx_take_error(APP_UART_CHANNEL_6) == 0U);
    assert(commTxErrorCalls == 1U);
}

static void test_frame_error_requests_recovery(void) {
    reset_all();

    huart6.ErrorCode = HAL_UART_ERROR_FE;
    HAL_UART_ErrorCallback(&huart6);

    assert(uart_rx_take_error(APP_UART_CHANNEL_6) != 0U);
}

/*
 * 송신 DMA만 죽고 수신은 그대로 돌고 있는 경우.
 * 예전에는 여기서도 수신을 통째로 재시작해 멀쩡한 바이트를 흘렸다.
 */
static void test_transmit_only_dma_error_leaves_receive_running(void) {
    reset_all();

    huart1.ErrorCode = HAL_UART_ERROR_DMA;
    huart1.hdmatx->ErrorCode = HAL_DMA_ERROR_TE;
    huart1.RxState = HAL_UART_STATE_BUSY_RX;
    HAL_UART_ErrorCallback(&huart1);

    assert(uart_rx_take_error(APP_UART_CHANNEL_1) == 0U);

    /* 송신 쪽 복구 경로는 그대로 통보받아야 한다. */
    assert(commTxErrorCalls == 1U);
}

/*
 * ST HAL의 UART_DMAError()는 송신 DMA 오류에도 수신을 함께 내린다.
 * 그때는 수신이 실제로 죽었으니 재시작해야 한다.
 */
static void test_transmit_dma_error_that_stopped_receive_requests_recovery(void) {
    reset_all();

    huart1.ErrorCode = HAL_UART_ERROR_DMA;
    huart1.hdmatx->ErrorCode = HAL_DMA_ERROR_TE;
    huart1.RxState = HAL_UART_STATE_READY;
    HAL_UART_ErrorCallback(&huart1);

    assert(uart_rx_take_error(APP_UART_CHANNEL_1) != 0U);
}

static void test_receive_dma_error_requests_recovery(void) {
    reset_all();

    huart6.ErrorCode = HAL_UART_ERROR_DMA;
    huart6.hdmarx->ErrorCode = HAL_DMA_ERROR_TE;
    huart6.RxState = HAL_UART_STATE_READY;
    HAL_UART_ErrorCallback(&huart6);

    assert(uart_rx_take_error(APP_UART_CHANNEL_6) != 0U);
}

int main(void) {
    test_restart_keeps_in_flight_transmit_alive();
    test_restart_targets_only_the_requested_channel();
    test_restart_rejects_unknown_channel();
    test_restart_reports_failed_receive_start();
    test_receive_error_requests_recovery();
    test_frame_error_requests_recovery();
    test_transmit_only_dma_error_leaves_receive_running();
    test_transmit_dma_error_that_stopped_receive_requests_recovery();
    test_receive_dma_error_requests_recovery();

    /* 수정 이후 어떤 경로에서도 HAL_UART_DMAStop은 호출되면 안 된다. */
    assert(dmaStopCalls == 0U);
    return 0;
}
