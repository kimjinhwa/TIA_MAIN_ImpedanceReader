/**
 * @file Ads1220.h
 * @brief TI ADS1220 24-bit ΔΣ ADC — SPI Mode 1 (CPOL=0, CPHA=1).
 *
 * SPI.begin() 후 Ads1220_begin() 호출. 공유 버스에서 AD5940·MCP CS는 HIGH 유지.
 * AIN0–AVSS 단일단(스키마 REF는 내부 기준 사용 전제).
 */
#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * CS 핀·DRDY·공유 CS(AD5940, MCP)·SPI 클럭 등록.
 * @param drdyPin 변환 완료 시 보통 LOW (내부 풀업·외부 풀다운 보드와 함께 INPUT_PULLUP 권장).
 */
void Ads1220_begin(uint8_t csPin, uint8_t drdyPin, uint8_t cs5940Pin, uint8_t mcpCsPin, uint32_t spiClockHz);

/** ADS1220·CS_5940·MCP CS 모두 비선택(HIGH). */
void Ads1220_end(void);

void Ads1220_reset(void);

/**
 * CONFIG0~3: MUX=AIN0–AVSS, 20 SPS, PGA=1, 내부 2.048V 기준(레지스터 기본값에 맞춤).
 * REFP0/REFN0가 GND로 묶인 보드에 맞는 일반 설정.
 */
void Ads1220_applyConfigAin0Avss(void);
void Ads1220_applyConfigAin1Avss(void);
bool Ads1220_applyConfigSingleEnded(uint8_t ainIndex);

/** START/SYNC (0x08) — 연속 변환 시작 등. */
void Ads1220_startSync(void);

/** DRDY가 준비될 때까지 (일반적으로 LOW=데이터 준비). 타임아웃 시 false. */
bool Ads1220_waitDrdy(uint32_t timeoutMs);

/** RDATA로 24비트 부호 확장 결과 읽기 (변환 준비 후 호출). */
int32_t Ads1220_readRaw(void);

uint8_t Ads1220_readReg8(uint8_t reg);
void Ads1220_writeReg8(uint8_t reg, uint8_t val);

/**
 * 내부 기준 2.048V, PGA 게인 gain(1,2,4,…에 맞춰 분모만 반영)일 때 대략 입력 전압(V).
 * 정확한 식은 데이터시트·보정에 따름.
 */
float Ads1220_rawToVolts(int32_t raw24, float vrefVolts, uint8_t pgaGain);
