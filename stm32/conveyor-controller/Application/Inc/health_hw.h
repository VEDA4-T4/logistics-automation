#ifndef HEALTH_HW_H
#define HEALTH_HW_H

#include <stdint.h>

#include "health_task.h"

/*
 * ============================================================================
 * HealthTask 하드웨어 접근 계층 (IWDG, 리셋원인 레지스터, .noinit 영속 기록)
 * ============================================================================
 *
 * health_task.c의 감시/판정 로직은 이 파일의 함수만 통해 하드웨어에 접근한다.
 * hc_sr04(센서 드라이버)/conveyor_motor_power(모터 STBY)와 같은 구조 -
 * 순수 로직과 레지스터 접근을 분리해서 health_task.c를 fake만으로 호스트에서
 * 테스트할 수 있게 한다.
 *
 * IWDG는 CubeMX(.ioc) 설정 없이 CMSIS 레지스터(IWDG->KR/PR/RLR)를 직접
 * 제어한다 - STM32 IWDG는 독립 RC 오실레이터(LSI)로 동작해 클럭트리 설정이
 * 필요 없고, KR에 0xCCCC를 쓰는 순간 하드웨어가 자동으로 LSI를 켠다.
 */

/* prescalerReg: IWDG_PR 레지스터 값(0=/4, 1=/8, 2=/16, 3=/32, 4=/64, 5=/128, 6·7=/256).
 * reload: IWDG_RLR(0~4095). timeout_ms ~= reload * (1 << (2+prescalerReg)) * 1000 / 32000. */
void HealthHw_IwdgStart(uint32_t prescalerReg, uint32_t reload);

/* IWDG 카운터를 reload 값으로 되돌린다("먹이 주기"). */
void HealthHw_IwdgRefresh(void);

/* 부팅 직후 1회 RCC->CSR의 리셋원인 플래그를 읽고 정리(clear)한다. */
health_reset_cause_t HealthHw_CaptureResetCause(void);

/*
 * 감시 대상 태스크의 남은 스택(워드 단위, uxTaskGetStackHighWaterMark 기준).
 * 태스크 핸들이 아직 없으면(시작 전) UINT32_MAX를 돌려줘 "판단 보류"로
 * 처리되게 한다.
 */
uint32_t HealthHw_GetStackHighWaterMark(health_task_id_t id);

/*
 * .noinit 영역(링커스크립트에 정의, IWDG/소프트 리셋에도 값 유지)에 있는
 * 마지막 치명 기록의 포인터. 항상 유효한 포인터를 돌려주며, 내용의 유효성은
 * magic/checksum으로 호출자가 검사한다(전원이 완전히 끊겼다 들어오면 값이
 * 사라지므로 그 경우 magic이 안 맞아 무효로 판정된다).
 */
health_persisted_record_t* HealthHw_GetPersistedRecord(void);

#endif /* HEALTH_HW_H */
