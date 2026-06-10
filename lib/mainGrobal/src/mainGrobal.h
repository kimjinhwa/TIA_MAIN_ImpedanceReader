#ifndef _MAIN_GROVAL_H
#define _MAIN_GROVAL_H
#include <Arduino.h>


#define SERIAL_RX1 26 // ok Serial1 통신 485모드버스통신으로 외부와의 인터페이스에 사용한다 
#define SERIAL_TX1 22 

// 기본 vSPI와 일치한다

#define IN_TH1              GPIO_NUM_34
#define IN_TH2              GPIO_NUM_35

#define EXT_485EN_1         GPIO_NUM_4  
// 5.0보드에서 추가한다.
#define RS_485ADD1          GPIO_NUM_33  
#define RS_485ADD2          GPIO_NUM_25  

#define RST_5940            GPIO_NUM_5  // 4951칩을 리셋하기 위함. 
#define RESET_5940          RST_5940            // 4951칩을 리셋하기 위함. 
// 4.0보드에서 변경한다.
//#define PORT1               GPIO_NUM_19
#define ADS1220_CS          GPIO_NUM_19
//#define PORT2               GPIO_NUM_18 
#define A23S08_CS            GPIO_NUM_18 

// 4.0보드에서 변경한다.
#define OPAMP_OFF_PORT               GPIO_NUM_27 // NOTUSE

// 4.0보드에서 변경한다.
//#define PORT4               GPIO_NUM_21
#define ADS1220_DR           GPIO_NUM_21

// 4.0보드에서 변경한다.
#define PORT5               GPIO_NUM_23  //Not use

#define READ_BATVOL         GPIO_NUM_36  //배터리 전압을 읽는다. SENSOR_VP
#define MISO                GPIO_NUM_12  
#define MOSI                GPIO_NUM_13  
#define SCK                 GPIO_NUM_14  
                
#define AD5940_ISR          GPIO_NUM_32  
#define CS_5940             GPIO_NUM_15  

#define ESP_INTR_FLAG_DEFAULT 0

#define MAX_INSTALLED_CELLS 20

typedef enum {
  IMPEDANCEMODE =0,  
  VOLTAGEMODE =1
} SetMode;
typedef struct
{
    char ssid[20];
    char ssid_password[10];
    char userid[10];
    char userpassword[10];                        
    uint8_t runMode; // 0: manual 1 : auto
    uint32_t IPADDRESS;   // 50 + 4 =54
    uint32_t GATEWAY;     // 54 + 4 = 58
    uint32_t SUBNETMASK;  // 58 + 4 = 62
    uint8_t modbusId;     // 62 + 1 = 63
    uint16_t installed_cells;     // 63 + 2 = 65
    uint16_t AlarmTemperature;    // 65 + 2 = 67
    uint16_t AlarmAmpere;    // 69
    uint16_t alarmHighCellVoltage;    // 71 
    uint16_t alarmLowCellVoltage;    // 73
    uint16_t cutoffHighCellVoltage;    // 75 
    uint16_t cutoffLowCellVoltage;    // 77
    uint16_t alarmDiffCellVoltage;    // 75 + 1 = 76
    int16_t voltageCompensation[20];// 76 + 80 =  156byte 
    int16_t impendanceCompensation[20];// 156 + 80 = 236
    int16_t baseVoltage[20];// 76 + 80 =  156byte 
    int16_t baseImpendance[20];// 156 + 80 = 236
    float real_Cal;  // 236+4 = 240
    float image_Cal; // 240 + 4 = 248
    uint8_t logLevel; // 240 + 4 = 248
    uint16_t startBatnumber;     // 63 + 2 = 65
    //AD5941 Parameter
    uint16_t ACVoltPP;
    uint16_t DCVolt;
    uint16_t SinFreq;
    uint16_t RcalLoopCount;
    uint8_t ImpedanceFactor;
    uint8_t VoltageFactor;
    uint8_t TemperatureFactor;
    uint16_t ImpedanceMeasurePeriod; /* 초, 0=기본 3600(1시간) */
    uint8_t impedanceAutoUpdateEnabled; /* 0=수동(자동 EEPROM 갱신 안함), 1=자동 갱신 */
    uint16_t useHoleCt; /* FC03/FC06 reg 9: 0=센서없음, 그 외 CT 정격(A) */
    int16_t ampereOffset; /* FC03/FC06 reg 11: 전류 오프셋(0.1A 단위) */
    uint16_t ampereGain; /* FC03/FC06 reg 12: 전류 게인(1000=1.000배) */
    uint16_t impedanceGainPermille;      /* 내부저항 전역 보정 게인(1000=1.000x) */
    int16_t impedanceOffsetCentiMohm;    /* 내부저항 전역 보정 오프셋(0.01mOhm) */
    uint16_t cellGain; /* FC03/FC06 reg 3: VOLTAGE_GAIN_RATIO x1000 */
    int16_t cellOffset; /* FC03/FC06 reg 4: VOLTAGE_OFFSET mV */
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
} nvsSystemSet;
extern nvsSystemSet systemDefaultValue;

/**
 * EEPROM 레이아웃: [0]=HEAD, [1 .. sizeof(nvsSystemSet)]=nvsSystemSet, [tail]=TAIL.
 * 구조체 크기가 바뀌면 tail 위치가 달라져 tail 검사 실패 → 기본값 재기록 권장.
 * EEPROM 읽기/쓰기 API는 C++ 전용 eepromNvs.hpp 를 포함할 것(mainGrobal.h는 .c에서도 쓰임).
 */
#define EEPROM_NV_MAGIC_HEAD 0x55u
#define EEPROM_NV_MAGIC_TAIL 0xAAu
#define EEPROM_NV_TAIL_BYTE_OFFSET (1u + sizeof(nvsSystemSet))
#define EEPROM_NV_RESERVED_BYTES   (2u + sizeof(nvsSystemSet))

typedef struct {
  time_t readTime; // 4byte
  float voltage;// 4byte
  float impendance;// 4byte
  int16_t temperature;// 2byte
  int16_t voltageCompensation;// 2byte
  int16_t impendanceCompensation;// 2byte
  int16_t baseVoltage;// 2byte
  int16_t baseImpendance;// 2byte
}_cell_value; // Total 18bte
extern _cell_value cellvalue[MAX_INSTALLED_CELLS];

typedef struct {
  uint16_t CellNo;
  time_t readTime; // 4byte
  float voltage;// 4byte
  int16_t temperature;// 2byte
  float impendance;// 4byte
}_cell_value_iv; // Total 18bte

typedef struct {
  time_t readTime; // 4byte
  float voltage[20];// 4byte
  float impendance[20];// 4byte
  int16_t temperature[20];// 2byte
}cell_logData_t; // Total 18bte

extern const int measuredImpedance_1[20];
extern const int measuredVoltage_1[20];
extern const int measuredImpedance_2[20];
extern const int measuredVoltage_2[20];

#ifdef __cplusplus
extern "C" {
#endif
void setVoltageReadMode(SetMode mode);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
bool readnWriteEEProm(bool writeMode);

/** IN_TH1(GPIO34), IN_TH2(GPIO35) NTC — Modbus FC04 주소 16·17 (0.1°C) */
extern int16_t ntcTemperatureC_x10[2];
void ntcTemperatureInit(void);
void ntcTemperatureUpdate(void);


int16_t ntcTemperatureGetCx10(uint8_t sensorIndex);

/** ADS1220 AIN2 CT 전류 — Modbus FC04 주소 18 (0.1A) */
extern int16_t packCurrentA_x10;
extern float packCurrentAin2Volts;
void ctCurrentInit(void);
void ctCurrentUpdate(void);
/** ADS1220 환산 원시 전류(A) */
float ctCurrentGetAmps(void);
/** Modbus FC04 reg18과 동일 — ampereOffset(0.1A) + ampereGain 적용 */
float ctCurrentGetCalibratedAmps(void);
#endif

#endif