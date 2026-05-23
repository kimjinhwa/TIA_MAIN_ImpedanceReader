#ifndef _MODBUSRTU_H
#define _MODBUSRTU_H
#include "ModbusServerRTU.h"

/** Modbus주소.md — FC03/FC06 홀딩 주소 50 (기준 저항 스캔 진행). 0=대기/완료, 1~N=측정 중 셀. */
extern uint16_t modbusReg50BaseImpProgress;

ModbusMessage FC01(ModbusMessage request);
ModbusMessage FC03(ModbusMessage request);
ModbusMessage FC04(ModbusMessage request);
ModbusMessage FC05(ModbusMessage request);
ModbusMessage FC06(ModbusMessage request);

ModbusMessage syncRequestCellModule(uint32_t token, uint8_t modbusId, uint8_t fCode,
                                    uint16_t startAddress, uint16_t len);

/** FC06 주소 50 쓰기 (main.cpp 기준 스캔 상태기 연동). */
void modbusOnFc06Reg50Write(uint16_t value);

/** loop()에서 호출 — 기준 저항 순차 측정. */
void modbusBaselineScanPoll(void);

bool modbusBaselineScanIsActive(void);
bool modbusHasCurrentSensor(void);
uint16_t modbusGetUseHoleCt(void);
int16_t modbusGetAmpereOffset(void);
uint16_t modbusGetAmpereGain(void);
uint16_t modbusGetCellGain(void);
int16_t modbusGetCellOffset(void);
void modbusSetUseHoleCt(uint16_t value);
void modbusSetAmpereOffset(int16_t value);
void modbusSetAmpereGain(uint16_t value);
void modbusSetCellGain(uint16_t value);
void modbusSetCellOffset(int16_t value);

void setErrorMessageToModbus(bool setError, const char *msg);

#endif
