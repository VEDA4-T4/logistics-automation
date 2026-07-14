#include "linetracer_app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    APP_STATE_IDLE = 0,
    APP_STATE_RAMP,
    APP_STATE_MANUAL_FORWARD,
    APP_STATE_MANUAL_PIVOT,
    APP_STATE_FOLLOW_FORWARD,
    APP_STATE_CORRECT_LEFT,
    APP_STATE_CORRECT_RIGHT,
    APP_STATE_LINE_LOST,
    APP_STATE_FAULT,
    APP_STATE_ESTOP
} AppState;

typedef enum {
    INPUT_VIRTUAL = 0,
    INPUT_REAL
} InputMode;

typedef enum {
    FAULT_NONE = 0,
    FAULT_LINE_TIMEOUT,
    FAULT_COMM_TIMEOUT,
    FAULT_ESTOP
} FaultCode;

typedef enum {
    ESTOP_SOURCE_NONE = 0,
    ESTOP_SOURCE_B1,
    ESTOP_SOURCE_PB12,
    ESTOP_SOURCE_UART
} EStopSource;

typedef struct {
    int8_t left_dir;
    int8_t right_dir;
    uint16_t left_pwm;
    uint16_t right_pwm;
    AppState state;
} MotionCommand;

typedef struct {
    TIM_HandleTypeDef *pwm_timer;
    UART_HandleTypeDef *uart;

    volatile AppState state;
    volatile uint8_t estop_latched;
    volatile uint8_t estop_event_pending;
    volatile EStopSource estop_source;
    volatile uint8_t fault_latched;
    volatile FaultCode fault;
    volatile uint8_t driving;
    InputMode input_mode;

    uint16_t base_pwm;
    uint16_t correction_pwm;
    int16_t left_trim;
    int16_t right_trim;
    uint32_t line_timeout_ms;

    volatile int8_t left_dir;
    volatile int8_t right_dir;
    volatile uint16_t left_pwm;
    volatile uint16_t right_pwm;

    uint8_t virtual_l;
    uint8_t virtual_r;
    uint8_t stable_l;
    uint8_t stable_r;
    uint8_t candidate_l;
    uint8_t candidate_r;
    uint8_t debounce_count;
    uint8_t last_report_l;
    uint8_t last_report_r;
    uint8_t line_lost_active;
    uint32_t line_lost_since;
    uint32_t last_line_sample;
    uint32_t last_command_tick;

    uint8_t test_id;
    uint8_t test_phase;
    uint8_t ramp_index;
    int8_t ramp_direction;
    uint32_t test_deadline;
    uint8_t test19_armed;

    uint32_t led_tick;
    uint8_t led_on;
} AppContext;

static AppContext app;

static uint8_t rx_byte;
static char rx_build[APP_COMMAND_BUFFER_SIZE];
static volatile uint8_t rx_build_index;
static char command_queue[APP_COMMAND_QUEUE_DEPTH][APP_COMMAND_BUFFER_SIZE];
static volatile uint8_t command_head;
static volatile uint8_t command_tail;
static volatile uint8_t command_overflow;

static void uart_write(const char *text)
{
    if ((app.uart == NULL) || (text == NULL)) {
        return;
    }
    (void)HAL_UART_Transmit(app.uart, (uint8_t *)text,
                            (uint16_t)strlen(text), 100U);
}

static void uart_printf(const char *format, ...)
{
    char buffer[192];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    uart_write(buffer);
}

static uint16_t clamp_pwm(int32_t value)
{
    if (value < 0) {
        return 0U;
    }
    if (value > (int32_t)APP_PWM_MAX) {
        return APP_PWM_MAX;
    }
    return (uint16_t)value;
}

static uint32_t motor_pwm_frequency_hz(void)
{
    uint32_t timer_clock = HAL_RCC_GetPCLK1Freq();
    uint32_t divider;

    /* APB1 prescaler가 1보다 크면 TIM3 clock은 PCLK1의 2배다. */
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U) {
        timer_clock *= 2U;
    }
    divider = (app.pwm_timer->Instance->PSC + 1U) *
              (app.pwm_timer->Instance->ARR + 1U);
    return (divider == 0U) ? 0U : (timer_clock / divider);
}

static int8_t physical_direction(int8_t logical, uint8_t reversed)
{
    if (logical == 0) {
        return 0;
    }
    return reversed ? (int8_t)-logical : logical;
}

static void set_left_direction(int8_t logical)
{
    int8_t direction = physical_direction(logical, APP_LEFT_MOTOR_REVERSED);

    if (direction > 0) {
        HAL_GPIO_WritePin(MOTOR_L_IN1_GPIO_Port, MOTOR_L_IN1_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(MOTOR_L_IN2_GPIO_Port, MOTOR_L_IN2_Pin, GPIO_PIN_RESET);
    } else if (direction < 0) {
        HAL_GPIO_WritePin(MOTOR_L_IN1_GPIO_Port, MOTOR_L_IN1_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_L_IN2_GPIO_Port, MOTOR_L_IN2_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(MOTOR_L_IN1_GPIO_Port, MOTOR_L_IN1_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_L_IN2_GPIO_Port, MOTOR_L_IN2_Pin, GPIO_PIN_RESET);
    }
}

static void set_right_direction(int8_t logical)
{
    int8_t direction = physical_direction(logical, APP_RIGHT_MOTOR_REVERSED);

    if (direction > 0) {
        HAL_GPIO_WritePin(MOTOR_R_IN1_GPIO_Port, MOTOR_R_IN1_Pin, GPIO_PIN_SET);
        HAL_GPIO_WritePin(MOTOR_R_IN2_GPIO_Port, MOTOR_R_IN2_Pin, GPIO_PIN_RESET);
    } else if (direction < 0) {
        HAL_GPIO_WritePin(MOTOR_R_IN1_GPIO_Port, MOTOR_R_IN1_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_R_IN2_GPIO_Port, MOTOR_R_IN2_Pin, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(MOTOR_R_IN1_GPIO_Port, MOTOR_R_IN1_Pin, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MOTOR_R_IN2_GPIO_Port, MOTOR_R_IN2_Pin, GPIO_PIN_RESET);
    }
}

static void motor_enable(uint8_t enabled)
{
    HAL_GPIO_WritePin(MOTOR_STBY_GPIO_Port, MOTOR_STBY_Pin,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void motor_stop_immediate(void)
{
    if (app.pwm_timer != NULL) {
        __HAL_TIM_SET_COMPARE(app.pwm_timer, TIM_CHANNEL_1, 0U);
        __HAL_TIM_SET_COMPARE(app.pwm_timer, TIM_CHANNEL_2, 0U);
    }
    set_left_direction(0);
    set_right_direction(0);
    motor_enable(0U);
    app.left_dir = 0;
    app.right_dir = 0;
    app.left_pwm = 0U;
    app.right_pwm = 0U;
}

static void drive_apply(int8_t left_dir, int8_t right_dir,
                        uint16_t left_pwm, uint16_t right_pwm)
{
    uint8_t reversing;

    if (app.estop_latched || app.fault_latched) {
        motor_stop_immediate();
        return;
    }

    left_pwm = clamp_pwm((int32_t)left_pwm + app.left_trim);
    right_pwm = clamp_pwm((int32_t)right_pwm + app.right_trim);

    reversing = (uint8_t)(((app.left_dir != 0) && (left_dir != 0) &&
                           (app.left_dir != left_dir)) ||
                          ((app.right_dir != 0) && (right_dir != 0) &&
                           (app.right_dir != right_dir)));
    if (reversing) {
        __HAL_TIM_SET_COMPARE(app.pwm_timer, TIM_CHANNEL_1, 0U);
        __HAL_TIM_SET_COMPARE(app.pwm_timer, TIM_CHANNEL_2, 0U);
        HAL_Delay(APP_DIRECTION_DEADTIME_MS);
    }

    set_left_direction(left_dir);
    set_right_direction(right_dir);
    __HAL_TIM_SET_COMPARE(app.pwm_timer, TIM_CHANNEL_1, left_pwm);
    __HAL_TIM_SET_COMPARE(app.pwm_timer, TIM_CHANNEL_2, right_pwm);
    motor_enable((uint8_t)((left_pwm > 0U) || (right_pwm > 0U)));

    app.left_dir = left_dir;
    app.right_dir = right_dir;
    app.left_pwm = left_pwm;
    app.right_pwm = right_pwm;
}

static void calculate_follow(uint8_t left, uint8_t right,
                             uint16_t base, uint16_t delta,
                             MotionCommand *output)
{
    output->left_dir = 1;
    output->right_dir = 1;

    if (left && right) {
        output->left_pwm = base;
        output->right_pwm = base;
        output->state = APP_STATE_FOLLOW_FORWARD;
    } else if (left && !right) {
        output->left_pwm = clamp_pwm((int32_t)base - delta);
        output->right_pwm = clamp_pwm((int32_t)base + delta);
        output->state = APP_STATE_CORRECT_LEFT;
    } else if (!left && right) {
        output->left_pwm = clamp_pwm((int32_t)base + delta);
        output->right_pwm = clamp_pwm((int32_t)base - delta);
        output->state = APP_STATE_CORRECT_RIGHT;
    } else {
        output->left_dir = 0;
        output->right_dir = 0;
        output->left_pwm = 0U;
        output->right_pwm = 0U;
        output->state = APP_STATE_LINE_LOST;
    }
}

static const char *state_name(AppState state)
{
    static const char *const names[] = {
        "IDLE", "RAMP", "MANUAL_FORWARD", "MANUAL_PIVOT",
        "FOLLOW_FORWARD", "CORRECT_LEFT", "CORRECT_RIGHT",
        "LINE_LOST", "FAULT", "ESTOP"
    };
    if ((unsigned)state >= (sizeof(names) / sizeof(names[0]))) {
        return "UNKNOWN";
    }
    return names[state];
}

static const char *fault_name(FaultCode fault)
{
    switch (fault) {
    case FAULT_NONE: return "NONE";
    case FAULT_LINE_TIMEOUT: return "LINE_TIMEOUT";
    case FAULT_COMM_TIMEOUT: return "COMM_TIMEOUT";
    case FAULT_ESTOP: return "ESTOP";
    default: return "UNKNOWN";
    }
}

static const char *estop_source_name(EStopSource source)
{
    switch (source) {
    case ESTOP_SOURCE_B1: return "B1_PC13";
    case ESTOP_SOURCE_PB12: return "ESTOP_PB12";
    case ESTOP_SOURCE_UART: return "UART";
    default: return "NONE";
    }
}

static void fault_stop(FaultCode fault)
{
    app.driving = 0U;
    app.fault_latched = 1U;
    app.fault = fault;
    app.state = APP_STATE_FAULT;
    motor_stop_immediate();
    uart_printf("ERR %s; RESET required\r\n", fault_name(fault));
}

static uint8_t read_line_pin(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_PinState raw = HAL_GPIO_ReadPin(port, pin);
    if (APP_LINE_SENSOR_ACTIVE_LOW) {
        return (uint8_t)(raw == GPIO_PIN_RESET);
    }
    return (uint8_t)(raw == GPIO_PIN_SET);
}

static void update_real_sensor(uint32_t now)
{
    uint8_t left;
    uint8_t right;

    if ((uint32_t)(now - app.last_line_sample) < APP_LINE_SAMPLE_PERIOD_MS) {
        return;
    }
    app.last_line_sample = now;

    left = read_line_pin(LINE_L_GPIO_Port, LINE_L_Pin);
    right = read_line_pin(LINE_R_GPIO_Port, LINE_R_Pin);

    if ((left == app.candidate_l) && (right == app.candidate_r)) {
        if (app.debounce_count < APP_LINE_DEBOUNCE_SAMPLES) {
            ++app.debounce_count;
        }
    } else {
        app.candidate_l = left;
        app.candidate_r = right;
        app.debounce_count = 1U;
    }

    if (app.debounce_count >= APP_LINE_DEBOUNCE_SAMPLES) {
        app.stable_l = app.candidate_l;
        app.stable_r = app.candidate_r;
    }
}

static void current_line(uint8_t *left, uint8_t *right)
{
    if (app.input_mode == INPUT_REAL) {
        *left = app.stable_l;
        *right = app.stable_r;
    } else {
        *left = app.virtual_l;
        *right = app.virtual_r;
    }
}

static void process_line_follow(uint32_t now)
{
    MotionCommand motion;
    uint8_t left;
    uint8_t right;

    if (!app.driving || app.estop_latched || app.fault_latched) {
        return;
    }

    current_line(&left, &right);
    if ((left != app.last_report_l) || (right != app.last_report_r)) {
        uart_printf("EVT LINE L%uR%u\r\n", left, right);
        app.last_report_l = left;
        app.last_report_r = right;
    }

    if (!left && !right) {
        if (!app.line_lost_active) {
            app.line_lost_active = 1U;
            app.line_lost_since = now;
            app.state = APP_STATE_LINE_LOST;
            uart_printf("WARN LINE_LOST; holding PWM L=%u R=%u for %lu ms\r\n",
                        app.left_pwm, app.right_pwm,
                        (unsigned long)app.line_timeout_ms);
        } else if ((uint32_t)(now - app.line_lost_since) >= app.line_timeout_ms) {
            fault_stop(FAULT_LINE_TIMEOUT);
        }
        return;
    }

    app.line_lost_active = 0U;
    calculate_follow(left, right, app.base_pwm, app.correction_pwm, &motion);
    app.state = motion.state;
    drive_apply(motion.left_dir, motion.right_dir,
                motion.left_pwm, motion.right_pwm);
}

static void stop_normal(void)
{
    app.driving = 0U;
    app.line_lost_active = 0U;
    app.test_id = 0U;
    motor_stop_immediate();
    if (app.estop_latched) {
        app.state = APP_STATE_ESTOP;
    } else if (app.fault_latched) {
        app.state = APP_STATE_FAULT;
    } else {
        app.state = APP_STATE_IDLE;
    }
}

static void start_follow(void)
{
    if (app.estop_latched || app.fault_latched) {
        uart_write("ERR latched; send RESET first\r\n");
        return;
    }
    app.driving = 1U;
    app.line_lost_active = 0U;
    app.last_report_l = 0xFFU;
    app.last_report_r = 0xFFU;
    process_line_follow(HAL_GetTick());
    uart_printf("OK GO mode=%s base=%u delta=%u\r\n",
                app.input_mode == INPUT_REAL ? "REAL" : "VIRTUAL",
                app.base_pwm, app.correction_pwm);
}

static void clear_latches(void)
{
    if (HAL_GPIO_ReadPin(ESTOP_GPIO_Port, ESTOP_Pin) == GPIO_PIN_RESET) {
        uart_write("ERR PB12 E-Stop is still pressed\r\n");
        return;
    }

    app.estop_latched = 0U;
    app.estop_event_pending = 0U;
    app.estop_source = ESTOP_SOURCE_NONE;
    app.fault_latched = 0U;
    app.fault = FAULT_NONE;
    app.test19_armed = 0U;
    stop_normal();
    uart_write("OK RESET; outputs remain stopped\r\n");
}

static void print_status(void)
{
    uint8_t left;
    uint8_t right;
    current_line(&left, &right);
    uart_printf("STATUS state=%s mode=%s drive=%u L%uR%u "
                "pwmL=%u pwmR=%u dirL=%d dirR=%d base=%u delta=%u "
                "trimL=%d trimR=%d timeout=%lu fault=%s estop=%s\r\n",
                state_name(app.state),
                app.input_mode == INPUT_REAL ? "REAL" : "VIRTUAL",
                app.driving, left, right,
                app.left_pwm, app.right_pwm,
                app.left_dir, app.right_dir,
                app.base_pwm, app.correction_pwm,
                app.left_trim, app.right_trim,
                (unsigned long)app.line_timeout_ms,
                fault_name(app.fault),
                estop_source_name(app.estop_source));
}

static void print_help(void)
{
    uart_write(
        "\r\nCommands\r\n"
        "  HELP | STATUS | CFG | PING\r\n"
        "  MODE VIRTUAL | MODE REAL\r\n"
        "  GO | STOP | RESET | ESTOP\r\n"
        "  L1R1 | L1R0 | L0R1 | L0R0       (virtual sensors)\r\n"
        "  SPD n | DELTA n | TRIML n | TRIMR n | TIMEOUT n\r\n"
        "  TEST 11 | TEST 12                (PWM LED ramp)\r\n"
        "  TEST 13 FWD|LEFT|RIGHT|PIVOT|STOP\r\n"
        "  TEST 14                           (LD2 pattern demo)\r\n"
        "  TEST 15 | TEST 16 | TEST 17      (virtual/C self-test)\r\n"
        "  TEST 18                           (automatic line timeout)\r\n"
        "  TEST 19                           (press B1 or PB12)\r\n"
        "Legacy: T1 T2 T3F T3L T3R T3P T3S, SPD:600\r\n\r\n");
}

static int parse_long_after(const char *command, const char *prefix, long *value)
{
    char *end;
    size_t length = strlen(prefix);

    if (strncmp(command, prefix, length) != 0) {
        return 0;
    }
    *value = strtol(command + length, &end, 10);
    while (*end == ' ') {
        ++end;
    }
    return (*end == '\0');
}

static uint8_t expect_motion(const char *label, uint8_t left, uint8_t right,
                             uint16_t base, uint16_t delta,
                             uint16_t expected_left, uint16_t expected_right,
                             AppState expected_state)
{
    MotionCommand motion;
    uint8_t pass;

    calculate_follow(left, right, base, delta, &motion);
    pass = (uint8_t)((motion.left_pwm == expected_left) &&
                     (motion.right_pwm == expected_right) &&
                     (motion.state == expected_state));
    uart_printf("[%s] %s -> L=%u R=%u state=%s\r\n",
                pass ? "PASS" : "FAIL", label,
                motion.left_pwm, motion.right_pwm,
                state_name(motion.state));
    return pass;
}

static void run_logic_self_test(void)
{
    unsigned pass = 0U;
    unsigned total = 0U;

    uart_write("[C SELFTEST] line-follow math\r\n");
    ++total; pass += expect_motion("600 L1R1", 1, 1, 600, 250,
                                  600, 600, APP_STATE_FOLLOW_FORWARD);
    ++total; pass += expect_motion("600 L1R0", 1, 0, 600, 250,
                                  350, 850, APP_STATE_CORRECT_LEFT);
    ++total; pass += expect_motion("600 L0R1", 0, 1, 600, 250,
                                  850, 350, APP_STATE_CORRECT_RIGHT);
    ++total; pass += expect_motion("300 clamp low", 1, 0, 300, 250,
                                  50, 550, APP_STATE_CORRECT_LEFT);
    ++total; pass += expect_motion("800 clamp high", 1, 0, 800, 250,
                                  550, 999, APP_STATE_CORRECT_LEFT);
    ++total; pass += expect_motion("lost", 0, 0, 600, 250,
                                  0, 0, APP_STATE_LINE_LOST);
    uart_printf("[C SELFTEST] RESULT %u/%u %s\r\n",
                pass, total, pass == total ? "PASS" : "FAIL");
}

static void start_ramp(uint8_t test_id)
{
    stop_normal();
    app.test_id = test_id;
    app.test_phase = 0U;
    app.ramp_index = 0U;
    app.ramp_direction = 1;
    app.test_deadline = HAL_GetTick();
    app.state = APP_STATE_RAMP;
    uart_printf("[TEST %u] %s PWM ramp 0..999..0; observe LED\r\n",
                test_id, test_id == 11U ? "LEFT PA6" : "RIGHT PA7");
}

static uint16_t ramp_value(uint8_t index)
{
    uint8_t max_index = (uint8_t)((APP_PWM_MAX + APP_RAMP_STEP - 1U) /
                                  APP_RAMP_STEP);
    uint32_t value;

    if (index >= max_index) {
        return APP_PWM_MAX;
    }
    value = (uint32_t)index * APP_RAMP_STEP;
    return (uint16_t)((value > APP_PWM_MAX) ? APP_PWM_MAX : value);
}

static void process_ramp_test(uint32_t now)
{
    uint8_t max_index = (uint8_t)((APP_PWM_MAX + APP_RAMP_STEP - 1U) /
                                  APP_RAMP_STEP);
    uint16_t pwm;

    if ((uint32_t)(now - app.test_deadline) < APP_RAMP_INTERVAL_MS) {
        return;
    }
    app.test_deadline = now;
    pwm = ramp_value(app.ramp_index);

    if (app.test_id == 11U) {
        drive_apply(1, 0, pwm, 0U);
    } else {
        drive_apply(0, 1, 0U, pwm);
    }
    uart_printf("TEST %u PWM=%u\r\n", app.test_id, pwm);

    if (app.ramp_direction > 0) {
        if (app.ramp_index >= max_index) {
            app.ramp_direction = -1;
            app.ramp_index = (uint8_t)(max_index - 1U);
        } else {
            ++app.ramp_index;
        }
    } else if (app.ramp_index > 0U) {
        --app.ramp_index;
    } else {
        uint8_t completed = app.test_id;
        stop_normal();
        uart_printf("[TEST %u] DONE - visual PASS if brightness changed smoothly\r\n",
                    completed);
    }
}

static void start_led_demo(void)
{
    stop_normal();
    app.test_id = 14U;
    app.test_phase = 0U;
    app.test_deadline = HAL_GetTick();
    app.led_tick = 0U;
    uart_write("[TEST 14] LD2: IDLE -> FORWARD -> CORRECTION -> LOST -> ESTOP\r\n");
}

static void process_led_demo(uint32_t now)
{
    static const char *const names[] = {
        "IDLE slow blink", "FORWARD solid", "CORRECTION fast blink",
        "LOST blink", "ESTOP very fast blink"
    };

    if ((uint32_t)(now - app.test_deadline) < 2000U) {
        return;
    }
    app.test_deadline = now;
    ++app.test_phase;
    if (app.test_phase < 5U) {
        uart_printf("[TEST 14] %s\r\n", names[app.test_phase]);
    } else {
        app.test_id = 0U;
        app.test_phase = 0U;
        app.state = APP_STATE_IDLE;
        uart_write("[TEST 14] DONE\r\n");
    }
}

static void start_timeout_test(void)
{
    stop_normal();
    app.input_mode = INPUT_VIRTUAL;
    app.virtual_l = 1U;
    app.virtual_r = 1U;
    app.test_id = 18U;
    app.test_phase = 0U;
    start_follow();
    app.test_id = 18U;
    app.test_deadline = HAL_GetTick();
    uart_write("[TEST 18] L1R1 for 0.5 s, then L0R0; wait for safe stop\r\n");
}

static void process_timeout_test(uint32_t now)
{
    if (app.test_phase == 0U) {
        if ((uint32_t)(now - app.test_deadline) >= 500U) {
            app.virtual_l = 0U;
            app.virtual_r = 0U;
            app.test_phase = 1U;
            app.test_deadline = now;
            uart_write("[TEST 18] injected L0R0\r\n");
        }
    } else if ((uint32_t)(now - app.test_deadline) >=
               (app.line_timeout_ms + 100U)) {
        uint8_t pass = (uint8_t)(app.fault_latched &&
                                 (app.fault == FAULT_LINE_TIMEOUT) &&
                                 (app.left_pwm == 0U) &&
                                 (app.right_pwm == 0U));
        app.test_id = 0U;
        uart_printf("[TEST 18] %s - PWM L=%u R=%u fault=%s\r\n",
                    pass ? "PASS" : "FAIL",
                    app.left_pwm, app.right_pwm, fault_name(app.fault));
        uart_write("Send RESET before the next drive test\r\n");
    }
}

static void manual_test(const char *action)
{
    if (strcmp(action, "STOP") != 0) {
        stop_normal();
    }

    if (strcmp(action, "FWD") == 0) {
        app.state = APP_STATE_MANUAL_FORWARD;
        drive_apply(1, 1, APP_TEST_MANUAL_PWM, APP_TEST_MANUAL_PWM);
    } else if (strcmp(action, "LEFT") == 0) {
        app.state = APP_STATE_CORRECT_LEFT;
        drive_apply(1, 1, APP_TEST_SLOW_PWM, APP_TEST_FAST_PWM);
    } else if (strcmp(action, "RIGHT") == 0) {
        app.state = APP_STATE_CORRECT_RIGHT;
        drive_apply(1, 1, APP_TEST_FAST_PWM, APP_TEST_SLOW_PWM);
    } else if (strcmp(action, "PIVOT") == 0) {
        app.state = APP_STATE_MANUAL_PIVOT;
        drive_apply(1, -1, APP_TEST_MANUAL_PWM, APP_TEST_MANUAL_PWM);
    } else if (strcmp(action, "STOP") == 0) {
        stop_normal();
    } else {
        uart_write("ERR TEST 13 action: FWD LEFT RIGHT PIVOT STOP\r\n");
        return;
    }
    uart_printf("[TEST 13] %s L(dir=%d,pwm=%u) R(dir=%d,pwm=%u)\r\n",
                action, app.left_dir, app.left_pwm,
                app.right_dir, app.right_pwm);
}

static void normalize_command(char *command)
{
    char *start = command;
    char *end;
    char *write;

    while (*start == ' ') {
        ++start;
    }
    if (start != command) {
        memmove(command, start, strlen(start) + 1U);
    }

    end = command + strlen(command);
    while ((end > command) && (end[-1] == ' ')) {
        --end;
    }
    *end = '\0';

    write = command;
    while (*write != '\0') {
        if ((*write >= 'a') && (*write <= 'z')) {
            *write = (char)(*write - ('a' - 'A'));
        }
        ++write;
    }
}

static uint8_t command_allowed_while_latched(const char *command)
{
    return (uint8_t)((strcmp(command, "HELP") == 0) ||
                     (strcmp(command, "STATUS") == 0) ||
                     (strcmp(command, "STAT") == 0) ||
                     (strcmp(command, "CFG") == 0) ||
                     (strcmp(command, "PING") == 0) ||
                     (strcmp(command, "RESET") == 0) ||
                     (strcmp(command, "RST") == 0) ||
                     (strcmp(command, "STOP") == 0) ||
                     (strcmp(command, "CMD:STATUS") == 0) ||
                     (strcmp(command, "CMD:RESET") == 0) ||
                     (strcmp(command, "CMD:STOP") == 0));
}

static void process_command(char *command)
{
    long value;
    app.last_command_tick = HAL_GetTick();
    normalize_command(command);

    if (command[0] == '\0') {
        return;
    }
    if ((app.estop_latched || app.fault_latched) &&
        !command_allowed_while_latched(command)) {
        uart_write("ERR latched; allowed: STATUS RESET HELP STOP\r\n");
        return;
    }

    if (strcmp(command, "HELP") == 0) {
        print_help();
    } else if ((strcmp(command, "STATUS") == 0) ||
               (strcmp(command, "STAT") == 0) ||
               (strcmp(command, "CMD:STATUS") == 0)) {
        print_status();
    } else if (strcmp(command, "CFG") == 0) {
        uart_printf("CFG PWM_MAX=%u PWM_HZ=%lu BASE=%u DELTA=%u "
                    "TRIML=%d TRIMR=%d LINE_ACTIVE_LOW=%u TIMEOUT=%lu\r\n",
                    APP_PWM_MAX, (unsigned long)motor_pwm_frequency_hz(),
                    app.base_pwm, app.correction_pwm,
                    app.left_trim, app.right_trim,
                    APP_LINE_SENSOR_ACTIVE_LOW,
                    (unsigned long)app.line_timeout_ms);
    } else if (strcmp(command, "PING") == 0) {
        uart_write("PONG\r\n");
    } else if ((strcmp(command, "RESET") == 0) ||
               (strcmp(command, "RST") == 0) ||
               (strcmp(command, "CMD:RESET") == 0)) {
        clear_latches();
    } else if ((strcmp(command, "STOP") == 0) ||
               (strcmp(command, "CMD:STOP") == 0)) {
        stop_normal();
        uart_write("OK STOP\r\n");
    } else if ((strcmp(command, "ESTOP") == 0) ||
               (strcmp(command, "CMD:ESTOP") == 0)) {
        app.estop_source = ESTOP_SOURCE_UART;
        app.estop_latched = 1U;
        app.estop_event_pending = 1U;
        app.fault = FAULT_ESTOP;
        app.fault_latched = 1U;
        app.state = APP_STATE_ESTOP;
        app.driving = 0U;
        motor_stop_immediate();
    } else if ((strcmp(command, "GO") == 0) ||
               (strncmp(command, "CMD:GO:", 7U) == 0)) {
        start_follow();
    } else if (strcmp(command, "MODE VIRTUAL") == 0) {
        stop_normal();
        app.input_mode = INPUT_VIRTUAL;
        app.virtual_l = 1U;
        app.virtual_r = 1U;
        uart_write("OK MODE VIRTUAL, default L1R1\r\n");
    } else if (strcmp(command, "MODE REAL") == 0) {
        stop_normal();
        app.input_mode = INPUT_REAL;
        app.debounce_count = 0U;
        uart_write("OK MODE REAL, reading PB4/PB5\r\n");
    } else if ((strlen(command) == 4U) &&
               (command[0] == 'L') && (command[2] == 'R') &&
               ((command[1] == '0') || (command[1] == '1')) &&
               ((command[3] == '0') || (command[3] == '1'))) {
        if (app.input_mode != INPUT_VIRTUAL) {
            uart_write("ERR sensor command requires MODE VIRTUAL\r\n");
        } else {
            app.virtual_l = (uint8_t)(command[1] - '0');
            app.virtual_r = (uint8_t)(command[3] - '0');
            process_line_follow(HAL_GetTick());
            uart_printf("ACK %s state=%s PWM L=%u R=%u\r\n",
                        command, state_name(app.state),
                        app.left_pwm, app.right_pwm);
        }
    } else if (parse_long_after(command, "SPD ", &value) ||
               parse_long_after(command, "SPD:", &value)) {
        if ((value < 0L) || (value > (long)APP_PWM_MAX)) {
            uart_write("ERR SPD range 0..999\r\n");
        } else {
            app.base_pwm = (uint16_t)value;
            uart_printf("OK BASE=%u\r\n", app.base_pwm);
        }
    } else if (parse_long_after(command, "DELTA ", &value)) {
        if ((value < 0L) || (value > (long)APP_PWM_MAX)) {
            uart_write("ERR DELTA range 0..999\r\n");
        } else {
            app.correction_pwm = (uint16_t)value;
            uart_printf("OK DELTA=%u\r\n", app.correction_pwm);
        }
    } else if (parse_long_after(command, "TRIML ", &value)) {
        if ((value < -500L) || (value > 500L)) {
            uart_write("ERR TRIML range -500..500\r\n");
        } else {
            app.left_trim = (int16_t)value;
            uart_printf("OK TRIML=%d\r\n", app.left_trim);
        }
    } else if (parse_long_after(command, "TRIMR ", &value)) {
        if ((value < -500L) || (value > 500L)) {
            uart_write("ERR TRIMR range -500..500\r\n");
        } else {
            app.right_trim = (int16_t)value;
            uart_printf("OK TRIMR=%d\r\n", app.right_trim);
        }
    } else if (parse_long_after(command, "TIMEOUT ", &value)) {
        if ((value < 100L) || (value > 10000L)) {
            uart_write("ERR TIMEOUT range 100..10000 ms\r\n");
        } else {
            app.line_timeout_ms = (uint32_t)value;
            uart_printf("OK TIMEOUT=%lu\r\n",
                        (unsigned long)app.line_timeout_ms);
        }
    } else if ((strcmp(command, "TEST 11") == 0) ||
               (strcmp(command, "T1") == 0)) {
        start_ramp(11U);
    } else if ((strcmp(command, "TEST 12") == 0) ||
               (strcmp(command, "T2") == 0)) {
        start_ramp(12U);
    } else if (strncmp(command, "TEST 13 ", 8U) == 0) {
        manual_test(command + 8U);
    } else if (strcmp(command, "T3F") == 0) {
        manual_test("FWD");
    } else if (strcmp(command, "T3L") == 0) {
        manual_test("LEFT");
    } else if (strcmp(command, "T3R") == 0) {
        manual_test("RIGHT");
    } else if (strcmp(command, "T3P") == 0) {
        manual_test("PIVOT");
    } else if (strcmp(command, "T3S") == 0) {
        manual_test("STOP");
    } else if (strcmp(command, "TEST 14") == 0) {
        start_led_demo();
    } else if (strcmp(command, "TEST 15") == 0) {
        stop_normal();
        app.input_mode = INPUT_VIRTUAL;
        app.virtual_l = 1U;
        app.virtual_r = 1U;
        uart_write("[TEST 15] PASS if L1R1/L1R0/L0R1/L0R0 each returns ACK\r\n");
    } else if ((strcmp(command, "TEST 16") == 0) ||
               (strcmp(command, "TEST 17") == 0) ||
               (strcmp(command, "SELFTEST") == 0)) {
        stop_normal();
        run_logic_self_test();
    } else if (strcmp(command, "TEST 18") == 0) {
        start_timeout_test();
    } else if (strcmp(command, "TEST 19") == 0) {
        stop_normal();
        app.test_id = 19U;
        app.test19_armed = 1U;
        uart_write("[TEST 19] Armed. Press Nucleo B1; later PB12 E-Stop uses same latch.\r\n");
    } else {
        uart_printf("ERR unknown command: %s (send HELP)\r\n", command);
    }
}

static uint8_t pop_command(char *destination)
{
    uint8_t available = 0U;

    __disable_irq();
    if (command_tail != command_head) {
        strcpy(destination, command_queue[command_tail]);
        command_tail = (uint8_t)((command_tail + 1U) % APP_COMMAND_QUEUE_DEPTH);
        available = 1U;
    }
    __enable_irq();
    return available;
}

static void process_tests(uint32_t now)
{
    if ((app.test_id == 11U) || (app.test_id == 12U)) {
        process_ramp_test(now);
    } else if (app.test_id == 14U) {
        process_led_demo(now);
    } else if (app.test_id == 18U) {
        process_timeout_test(now);
    }
}

static void update_status_led(uint32_t now)
{
    uint32_t period = 1000U;
    uint8_t solid = 0U;

    if (app.estop_latched) {
        period = 80U;
    } else if (app.test_id == 14U) {
        switch (app.test_phase) {
        case 0: period = 1000U; break;
        case 1: solid = 1U; break;
        case 2: period = 200U; break;
        case 3: period = 400U; break;
        default: period = 80U; break;
        }
    } else {
        switch (app.state) {
        case APP_STATE_FOLLOW_FORWARD:
        case APP_STATE_MANUAL_FORWARD:
            solid = 1U;
            break;
        case APP_STATE_CORRECT_LEFT:
        case APP_STATE_CORRECT_RIGHT:
            period = 200U;
            break;
        case APP_STATE_LINE_LOST:
        case APP_STATE_FAULT:
            period = 400U;
            break;
        case APP_STATE_MANUAL_PIVOT:
            period = 150U;
            break;
        case APP_STATE_RAMP:
            period = 300U;
            break;
        default:
            period = 1000U;
            break;
        }
    }

    if (solid) {
        app.led_on = 1U;
        HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
    } else if ((uint32_t)(now - app.led_tick) >= period) {
        app.led_tick = now;
        app.led_on = (uint8_t)!app.led_on;
        HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin,
                          app.led_on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

void App_Init(TIM_HandleTypeDef *motor_pwm_timer,
              UART_HandleTypeDef *command_uart)
{
    memset(&app, 0, sizeof(app));
    app.pwm_timer = motor_pwm_timer;
    app.uart = command_uart;
    app.state = APP_STATE_IDLE;
    app.input_mode = INPUT_VIRTUAL;
    app.base_pwm = APP_DEFAULT_BASE_PWM;
    app.correction_pwm = APP_DEFAULT_CORRECTION_PWM;
    app.left_trim = APP_DEFAULT_LEFT_TRIM;
    app.right_trim = APP_DEFAULT_RIGHT_TRIM;
    app.line_timeout_ms = APP_LINE_LOST_TIMEOUT_MS;
    app.virtual_l = 1U;
    app.virtual_r = 1U;
    app.last_report_l = 0xFFU;
    app.last_report_r = 0xFFU;
    app.last_command_tick = HAL_GetTick();

    if (HAL_TIM_PWM_Start(app.pwm_timer, TIM_CHANNEL_1) != HAL_OK) {
        Error_Handler();
    }
    if (HAL_TIM_PWM_Start(app.pwm_timer, TIM_CHANNEL_2) != HAL_OK) {
        Error_Handler();
    }
    motor_stop_immediate();

    rx_build_index = 0U;
    command_head = 0U;
    command_tail = 0U;
    command_overflow = 0U;
    if (HAL_UART_Receive_IT(app.uart, &rx_byte, 1U) != HAL_OK) {
        Error_Handler();
    }

    uart_write("\r\nLINETRACER F401RE TEST FW READY\r\n");
    uart_write("Default: VIRTUAL sensors, motors SAFE/OFF. Send HELP.\r\n");
    if ((app.pwm_timer->Init.Period != APP_PWM_MAX) ||
        (app.pwm_timer->Init.Prescaler != APP_EXPECTED_TIM3_PRESCALER)) {
        uart_printf("WARN dev.ioc TIM3 differs: PSC=%lu ARR=%lu (expected %u/%u)\r\n",
                    (unsigned long)app.pwm_timer->Init.Prescaler,
                    (unsigned long)app.pwm_timer->Init.Period,
                    APP_EXPECTED_TIM3_PRESCALER, APP_PWM_MAX);
    }
    if (app.uart->Init.BaudRate != APP_UART_BAUDRATE) {
        uart_printf("WARN dev.ioc USART6 baud=%lu (expected %u)\r\n",
                    (unsigned long)app.uart->Init.BaudRate,
                    APP_UART_BAUDRATE);
    }
}

void App_Task(void)
{
    char command[APP_COMMAND_BUFFER_SIZE];
    uint32_t now = HAL_GetTick();

    if (command_overflow) {
        __disable_irq();
        command_overflow = 0U;
        __enable_irq();
        uart_write("ERR UART command queue overflow\r\n");
    }

    while (pop_command(command)) {
        process_command(command);
    }

    if (app.estop_event_pending) {
        EStopSource source;
        __disable_irq();
        app.estop_event_pending = 0U;
        source = app.estop_source;
        __enable_irq();
        uart_printf("EVT ESTOP source=%s; outputs disabled; RESET required\r\n",
                    estop_source_name(source));
        if (app.test19_armed) {
            app.test19_armed = 0U;
            app.test_id = 0U;
            uart_write("[TEST 19] PASS\r\n");
        }
    }

    if (app.input_mode == INPUT_REAL) {
        update_real_sensor(now);
    }
    process_tests(now);
    process_line_follow(now);

#if APP_COMM_WATCHDOG_ENABLE
    if (app.driving &&
        ((uint32_t)(now - app.last_command_tick) >=
         APP_COMM_WATCHDOG_TIMEOUT_MS)) {
        fault_stop(FAULT_COMM_TIMEOUT);
    }
#endif

    update_status_led(now);
}

void App_UartRxCompleteFromISR(UART_HandleTypeDef *huart)
{
    uint8_t next;
    uint8_t i;

    if ((app.uart == NULL) || (huart->Instance != app.uart->Instance)) {
        return;
    }

    if ((rx_byte == '\r') || (rx_byte == '\n')) {
        if (rx_build_index > 0U) {
            rx_build[rx_build_index] = '\0';
            next = (uint8_t)((command_head + 1U) % APP_COMMAND_QUEUE_DEPTH);
            if (next == command_tail) {
                command_overflow = 1U;
            } else {
                for (i = 0U; i <= rx_build_index; ++i) {
                    command_queue[command_head][i] = rx_build[i];
                }
                command_head = next;
            }
            rx_build_index = 0U;
        }
    } else if ((rx_byte == 8U) || (rx_byte == 127U)) {
        if (rx_build_index > 0U) {
            --rx_build_index;
        }
    } else if ((rx_byte >= 32U) && (rx_byte <= 126U)) {
        if (rx_build_index < (APP_COMMAND_BUFFER_SIZE - 1U)) {
            rx_build[rx_build_index++] = (char)rx_byte;
        } else {
            rx_build_index = 0U;
            command_overflow = 1U;
        }
    }

    (void)HAL_UART_Receive_IT(app.uart, &rx_byte, 1U);
}

void App_UartErrorFromISR(UART_HandleTypeDef *huart)
{
    if ((app.uart != NULL) && (huart->Instance == app.uart->Instance)) {
        rx_build_index = 0U;
        (void)HAL_UART_Receive_IT(app.uart, &rx_byte, 1U);
    }
}

void App_EStopFromISR(uint16_t gpio_pin)
{
    EStopSource source;

    if (gpio_pin == B1_Pin) {
        source = ESTOP_SOURCE_B1;
    } else if (gpio_pin == ESTOP_Pin) {
        source = ESTOP_SOURCE_PB12;
    } else {
        return;
    }

    app.estop_source = source;
    app.estop_latched = 1U;
    app.estop_event_pending = 1U;
    app.fault_latched = 1U;
    app.fault = FAULT_ESTOP;
    app.driving = 0U;
    app.state = APP_STATE_ESTOP;

    /* UART 출력 없이 ISR 안에서 즉시 PWM과 STBY를 차단한다. */
    if (app.pwm_timer != NULL) {
        __HAL_TIM_SET_COMPARE(app.pwm_timer, TIM_CHANNEL_1, 0U);
        __HAL_TIM_SET_COMPARE(app.pwm_timer, TIM_CHANNEL_2, 0U);
    }
    HAL_GPIO_WritePin(MOTOR_STBY_GPIO_Port, MOTOR_STBY_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC,
                      MOTOR_L_IN1_Pin | MOTOR_L_IN2_Pin |
                      MOTOR_R_IN1_Pin | MOTOR_R_IN2_Pin,
                      GPIO_PIN_RESET);
    app.left_pwm = 0U;
    app.right_pwm = 0U;
    app.left_dir = 0;
    app.right_dir = 0;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    App_UartRxCompleteFromISR(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    App_UartErrorFromISR(huart);
}

void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin)
{
    App_EStopFromISR(gpio_pin);
}
