/**
 * @file Mcp23s08.h
 * @brief MCP23S08-E/SS SPI 8-bit I/O expander (A2=A1=A0=GND).
 *
 * SPI must be initialized (SPI.begin) before Mcp23s08_begin().
 */
#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * MCP CS + 공유 버스에서 비선택으로 둘 다른 슬레이브 CS(AD5940, ADS1220) 등록.
 * write 시 CS_5940·ADS1220_CS를 HIGH로 둔 뒤 MCP만 선택.
 */
void Mcp23s08_begin(uint8_t mcpCsPin, uint8_t cs5940Pin, uint8_t ads1220CsPin, uint32_t spiClockHz);

/** MCP·CS_5940·ADS1220_CS 모두 비선택(HIGH). MCP 구간 종료 후 호출 권장. */
void Mcp23s08_end(void);

/** IODIR=0x00: GP0~GP7 출력 */
void Mcp23s08_initOutputsAll(void);

/** 임의 레지스터 쓰기 (SPI write opcode 0x40, HW addr 000) */
void Mcp23s08_writeReg(uint8_t reg, uint8_t val);

/**
 * OLAT에 pattern을 그대로 기록 (0→0x00, 1→0x01, 2→0x02 …).
 * 비트 마스크가 아니라 바이트 값 그대로입니다.
 */
void Mcp23s08_setOutput(uint8_t pattern);

/**
 * 테스트: 각 GP마다 해당 비트만 LOW(나머지 HIGH), delayMs 간격, rounds 바깥 루프.
 */
void Mcp23s08_testPortWalk(unsigned rounds, uint32_t delayMs);
