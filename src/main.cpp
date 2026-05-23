#include <Arduino.h>
#include <sys/time.h>
#include <SPI.h>
#include <EEPROM.h>
#include <wifi.h>
#include <driver/adc.h>
#include "filesystem.h"
#include "mainGrobal.h"
#include "eepromNvs.hpp"
#include "SimpleCLI.h"


#include "stdio.h"
//#include "ADuCM3029.h"
#include "AD5940.h"
//#include "HardwareSerialExtention.h"
#include "HardwareSerial.h"
#include <BluetoothSerial.h>
#include "myBlueTooth.h"
#include "NetworkTask.h"
#include "ModbusClientRTU.h"
#include "modbusRtu.h"
#include "batDeviceInterface.h"
#include <Mcp23s08.h>
#include <Ads1220.h>
#include "ntcTemperature.h"
#include "ctCurrent.h"
#include "restApi.h"
#include "dataSync.h"

// #include <esp_int_wdt.h>
// #include <esp_task.h>
#include <esp_task_wdt.h>
#include <esp_log.h>

#define MAIN_POWEROFF HIGH
#define MAIN_POWERON LOW 
#define WDT_TIMEOUT 100 
#define IMP_MEASURE_PERIOD_DEFAULT_SEC 3600
#define IMP_EEPROM_CHANGE_DEFAULT_PERCENT 3u
#define IMP_READ_MAX_DEFAULT 60u
#define IMP_READ_MAX_MAX 120u
#define IMP_STABLE_WINDOW_DEFAULT 5u
#define IMP_STABLE_WINDOW_MAX 20u
#define IMP_STABLE_REL_TOL_DEFAULT_PERCENT 3u
#define IMP_POST_STABLE_SAMPLES_DEFAULT 5u
#define IMP_POST_STABLE_SAMPLES_MAX 20u
#define IMP_MAG_MIN_VALID_MOHM_DEFAULT_DECI 50u
// 기본 vSPI와 일치한다
#define VSPI_MISO   MISO  // IO19
#define VSPI_MOSI   MOSI  // IO 23
#define VSPI_SCLK   SCK   // IO 18
#define VSPI_SS     15    // IO 15

#define NUM_VALUES 21


//SPIClass SPI;
static const int spiClk = 1000000; // 1 MHz
static char TAG[] ="Main";

static TaskHandle_t h_networkTask = nullptr;
static TaskHandle_t s_serviceTaskHandle = nullptr;
/* legacy symbol: referenced by fileSystem.cpp (df command) */
TaskHandle_t *h_pxblueToothTask = nullptr;
static volatile bool s_apRestartRequested = false;
static bool s_wifiEventHandlerRegistered = false;
nvsSystemSet systemDefaultValue;

ModbusServerRTU external485(2000,EXT_485EN_1);

uint32_t request_time;
uint16_t values[2];
uint16_t cellModbusIdReceived;

uint8_t selecectedCellNumber =0;
volatile bool isAd5940Interrupt = false;

_cell_value cellvalue[MAX_INSTALLED_CELLS];

extern SimpleCLI simpleCli;
uint16_t startBatnumber=1;

BluetoothSerial SerialBT;
using LogVprintfFn = int (*)(const char *, va_list);
static LogVprintfFn s_prevLogVprintf = nullptr;
static volatile bool s_btLogMirrorEnabled = false;
static volatile bool s_espLogEnabled = true;
static bool s_lastEspLogEnabledApplied = true;

BatDeviceInterface batDevice;


//void AD5940_ShutDown();

float AD5940_calibration(float *real , float *image);
float AD5940_readImpMagnitude(fImpCar_Type *pCarOut);
void changeAD5940ToMeasurement(bool bChange);
uint8_t get485Address();

static int btMirrorVprintf(const char *fmt, va_list args)
{
  if (!s_espLogEnabled)
    return 0;

  va_list copyForPrev;
  va_copy(copyForPrev, args);
  const int ret = s_prevLogVprintf ? s_prevLogVprintf(fmt, copyForPrev) : vprintf(fmt, copyForPrev);
  va_end(copyForPrev);

  if (s_btLogMirrorEnabled && SerialBT.connected() && fmt)
  {
    char line[256];
    va_list copyForBt;
    va_copy(copyForBt, args);
    const int n = vsnprintf(line, sizeof(line), fmt, copyForBt);
    va_end(copyForBt);
    if (n > 0)
    {
      const size_t outLen = (size_t)((n < (int)(sizeof(line) - 1)) ? n : (int)(sizeof(line) - 1));
      SerialBT.write((const uint8_t *)line, outLen);
    }
  }

  return ret;
}

static void installBtLogMirror(void)
{
  if (!s_prevLogVprintf)
    s_prevLogVprintf = esp_log_set_vprintf(btMirrorVprintf);
}

bool btLogMirrorIsEnabled(void)
{
  return s_btLogMirrorEnabled;
}

void btLogMirrorSetEnabled(bool enabled)
{
  s_btLogMirrorEnabled = enabled;
}

bool espLogIsEnabled(void)
{
  return s_espLogEnabled;
}

void espLogSetEnabled(bool enabled)
{
  s_espLogEnabled = enabled;
  esp_log_level_set("*", enabled ? ESP_LOG_INFO : ESP_LOG_NONE);
  s_lastEspLogEnabledApplied = enabled;
}

void pinsetup()
{
    pinMode(READ_BATVOL, INPUT);
    pinMode(AD5940_ISR, INPUT);

    pinMode(EXT_485EN_1, OUTPUT);
    digitalWrite(EXT_485EN_1, LOW);

    pinMode(RST_5940, OUTPUT);
    digitalWrite(RST_5940, HIGH);

    pinMode(RS_485ADD1, INPUT_PULLUP);
    pinMode(RS_485ADD2, INPUT_PULLUP);

    pinMode(ADS1220_CS, OUTPUT);
    pinMode(A23S08_CS, OUTPUT);
    pinMode(PORT3, OUTPUT); // NOT USE
    pinMode(ADS1220_CS, OUTPUT);
    pinMode(ADS1220_DR, INPUT_PULLUP); /* DRDY: 변환 준비 시 보통 LOW */
    pinMode(PORT5, OUTPUT);  //NOT USE
    digitalWrite(ADS1220_CS, HIGH);
    digitalWrite(A23S08_CS, HIGH);  /* CS active-low: idle = HIGH */
    digitalWrite(ADS1220_CS, HIGH);
    digitalWrite(PORT3, HIGH); //NOT USE
    digitalWrite(PORT5, HIGH); //NOT USE

    pinMode(CS_5940, OUTPUT);
    digitalWrite(CS_5940, HIGH);
    pinMode(RST_5940, OUTPUT);
    digitalWrite(RST_5940, HIGH);

}
void AD5940_Main(void *parameters);

#ifdef WIFI_AP_MODE
static void serviceTask(void *parameters)
{
  (void)parameters;
  myBlueTooth blueTooth;
  simpleCli.inputStream = &Serial;
  uint32_t previousBtSwitchMs = 0;
  const uint32_t btSwitchIntervalMs = 5000;

  for (;;)
  {
#ifdef WIFI_AP_MODE
    restApiHandle();
#endif
    blueTooth.readInputSerialBT();

    const uint32_t nowMs = millis();
    if ((nowMs - previousBtSwitchMs) > btSwitchIntervalMs)
    {
      previousBtSwitchMs = nowMs;
      if (SerialBT.connected())
      {
        lsFile.setOutputStream(&SerialBT);
        simpleCli.inputStream = &SerialBT;
        simpleCli.outputStream = &SerialBT;
      }
      else
      {
        lsFile.setOutputStream(&Serial);
        simpleCli.outputStream = &Serial;
        simpleCli.inputStream = &Serial;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif

/** EEPROM 웹 로그인 계정 — Serial/로그 (Serial.begin 및 WiFi AP 이후 호출). */
static void printWebLoginCredentials(void)
{
  ESP_LOGI(TAG, "===== Web login (EEPROM) =====");
  ESP_LOGI(TAG, "  User ID      : %s", systemDefaultValue.userid);
  ESP_LOGI(TAG, "  User password: %s", systemDefaultValue.userpassword);
#ifdef WIFI_AP_MODE
  ESP_LOGI(TAG, "  Web URL(AP)  : http://%s/login.html", WiFi.softAPIP().toString().c_str());
#endif

  Serial.println();
  Serial.println("===== Web login (EEPROM) =====");
  Serial.printf("  User ID      : %s\r\n", systemDefaultValue.userid);
  Serial.printf("  User password: %s\r\n", systemDefaultValue.userpassword);
#ifdef WIFI_AP_MODE
  Serial.printf("  Web URL(AP)  : http://%s/login.html\r\n", WiFi.softAPIP().toString().c_str());
#endif
  Serial.println("================================");
  Serial.println();
}

static bool startApOnly(void)
{
  static const char *const kApPassword = "87654321";
  static const IPAddress kApIp(192, 168, 11, 1);
  static const IPAddress kApGw(0, 0, 0, 0);
  static const IPAddress kApMask(255, 255, 255, 0);

  WiFi.mode(WIFI_MODE_AP);
  if (!WiFi.softAPConfig(kApIp, kApGw, kApMask)) {
    ESP_LOGW(TAG, "WiFi.softAPConfig failed");
  }

  String apSsid = "POSCOIMP_";
  apSsid += get485Address();
  const bool ok = WiFi.softAP(apSsid.c_str(), kApPassword);
  if (ok) {
    ESP_LOGI(TAG, "WiFi AP started: SSID=%s  IP=%s",
             apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  } else {
    ESP_LOGE(TAG, "WiFi.softAP failed: SSID=%s", apSsid.c_str());
  }
  return ok;
}

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
  (void)info;
#ifdef WIFI_AP_MODE
  if (event == ARDUINO_EVENT_WIFI_AP_STOP)
  {
    s_apRestartRequested = true;
    ESP_LOGW(TAG, "WiFi event: AP_STOP detected, restart requested");
  }
#endif
}

void wifiApmodeConfig()
{
#ifdef WIFI_AP_MODE
  if (!s_wifiEventHandlerRegistered)
  {
    WiFi.onEvent(onWifiEvent);
    s_wifiEventHandlerRegistered = true;
  }
  startApOnly();
  restApiInit();
#endif
}
bool readnWriteEEProm(bool writeMode)
{
  if (writeMode)
  {
    eepromNvsWriteBlock(&systemDefaultValue);
    modbusSaveCalibToEeprom();
    const bool saved = EEPROM.commit();
    if (saved)
      EEPROM.readBytes(1, (byte *)&systemDefaultValue, sizeof(nvsSystemSet));
    return saved;
  }
  uint8_t ipaddr1;
  dataSyncLockSystemConfig();
  if (!eepromNvsBlockLooksValid())
  {
    systemDefaultValue.runMode = 0;  // 0: manual 0x01 : onlyVoltate Audo, 0x03 : Voltage & Impedance 
    systemDefaultValue.AlarmAmpere = 2000;  // 200A
    systemDefaultValue.alarmDiffCellVoltage = 200;  //200mV
    systemDefaultValue.alarmHighCellVoltage = 1450;  //14.5V
    systemDefaultValue.alarmLowCellVoltage = 850; //8.5V
    systemDefaultValue.AlarmTemperature = 65;
    systemDefaultValue.cutoffHighCellVoltage = 14800;
    systemDefaultValue.cutoffLowCellVoltage = 6500;
    systemDefaultValue.GATEWAY =(uint32_t)IPAddress(192, 168, 0, 1); 
    systemDefaultValue.IPADDRESS =(uint32_t)IPAddress(192, 168, 0, 201); 
    systemDefaultValue.modbusId = 1;
    strncpy(systemDefaultValue.ssid ,"iptime_mbhong",20);
    strncpy(systemDefaultValue.ssid_password,"",10);
    systemDefaultValue.SUBNETMASK =(uint32_t)IPAddress(255, 255, 255, 0); 
    systemDefaultValue.installed_cells= 15;
    strncpy(systemDefaultValue.userid,"admin",10);
    strncpy(systemDefaultValue.userpassword,"admin",10);
    for(int i=0;i<20;i++){
      systemDefaultValue.voltageCompensation[i]=0;
      systemDefaultValue.impendanceCompensation[i]=0;
    }
    systemDefaultValue.real_Cal = -33410.0f;
    systemDefaultValue.image_Cal = 35511.0f;
    systemDefaultValue.logLevel = ESP_LOG_INFO;
    systemDefaultValue.startBatnumber = 1;
    systemDefaultValue.ImpedanceMeasurePeriod = 3600; /* 초, 0이면 런타임 기본 3600 */
    systemDefaultValue.ImpedanceFactor = IMP_EEPROM_CHANGE_DEFAULT_PERCENT; /* EEPROM 저장 임계치(%) */
    systemDefaultValue.ACVoltPP = IMP_READ_MAX_DEFAULT;                      /* 최대 읽기 횟수 */
    systemDefaultValue.DCVolt = IMP_STABLE_WINDOW_DEFAULT;                   /* 안정 판단 윈도우 */
    systemDefaultValue.VoltageFactor = IMP_STABLE_REL_TOL_DEFAULT_PERCENT;   /* 안정 판단 허용치(%) */
    systemDefaultValue.TemperatureFactor = IMP_POST_STABLE_SAMPLES_DEFAULT;  /* 안정 후 추가 샘플 수 */
    systemDefaultValue.RcalLoopCount = IMP_MAG_MIN_VALID_MOHM_DEFAULT_DECI;  /* 최소 유효 mOhm (x10) */
    eepromNvsWriteBlock(&systemDefaultValue);
    EEPROM.commit();
  }
  EEPROM.readBytes(1, (byte *)&systemDefaultValue, sizeof(nvsSystemSet));
  systemDefaultValue.userid[sizeof(systemDefaultValue.userid) - 1] = '\0';
  systemDefaultValue.userpassword[sizeof(systemDefaultValue.userpassword) - 1] = '\0';
  if (systemDefaultValue.ImpedanceMeasurePeriod == 0)
    systemDefaultValue.ImpedanceMeasurePeriod = IMP_MEASURE_PERIOD_DEFAULT_SEC;
  if (systemDefaultValue.ImpedanceFactor == 0 || systemDefaultValue.ImpedanceFactor > 100)
    systemDefaultValue.ImpedanceFactor = IMP_EEPROM_CHANGE_DEFAULT_PERCENT;
  if (systemDefaultValue.ACVoltPP == 0 || systemDefaultValue.ACVoltPP > IMP_READ_MAX_MAX)
    systemDefaultValue.ACVoltPP = IMP_READ_MAX_DEFAULT;
  if (systemDefaultValue.DCVolt == 0 || systemDefaultValue.DCVolt > IMP_STABLE_WINDOW_MAX)
    systemDefaultValue.DCVolt = IMP_STABLE_WINDOW_DEFAULT;
  if (systemDefaultValue.VoltageFactor == 0 || systemDefaultValue.VoltageFactor > 20)
    systemDefaultValue.VoltageFactor = IMP_STABLE_REL_TOL_DEFAULT_PERCENT;
  if (systemDefaultValue.TemperatureFactor == 0 || systemDefaultValue.TemperatureFactor > IMP_POST_STABLE_SAMPLES_MAX)
    systemDefaultValue.TemperatureFactor = IMP_POST_STABLE_SAMPLES_DEFAULT;
  if (systemDefaultValue.RcalLoopCount == 0 || systemDefaultValue.RcalLoopCount > 10000)
    systemDefaultValue.RcalLoopCount = IMP_MAG_MIN_VALID_MOHM_DEFAULT_DECI;
  if(systemDefaultValue.startBatnumber > systemDefaultValue.installed_cells  )
    systemDefaultValue.startBatnumber = systemDefaultValue.installed_cells;
  if(systemDefaultValue.startBatnumber == 0) 
  startBatnumber = 1;
  ESP_LOGI(TAG, "Installed cells: %d", systemDefaultValue.installed_cells);
  ESP_LOGI(TAG, "Start bat number: %d", systemDefaultValue.startBatnumber);
  ESP_LOGI(TAG, "SSID: %s", systemDefaultValue.ssid);
  ESP_LOGI(TAG, "SSID password: %s", systemDefaultValue.ssid_password);
  ESP_LOGI(TAG, "Run mode: %d", systemDefaultValue.runMode);
  ESP_LOGI(TAG, "Modbus ID: %d", systemDefaultValue.modbusId);
  ESP_LOGI(TAG, "Impedance EEPROM change percent: %u%%", (unsigned)systemDefaultValue.ImpedanceFactor);
  ESP_LOGI(TAG, "Impedance read max: %u", (unsigned)systemDefaultValue.ACVoltPP);
  ESP_LOGI(TAG, "Impedance stable window: %u", (unsigned)systemDefaultValue.DCVolt);
  ESP_LOGI(TAG, "Impedance measure period: %us", (unsigned)systemDefaultValue.ImpedanceMeasurePeriod);
  ESP_LOGI(TAG, "Impedance stable tolerance: %u%%", (unsigned)systemDefaultValue.VoltageFactor);
  ESP_LOGI(TAG, "Impedance post samples: %u", (unsigned)systemDefaultValue.TemperatureFactor);
  ESP_LOGI(TAG, "Impedance min valid: %.1f mOhm", (float)systemDefaultValue.RcalLoopCount / 10.0f);
  ESP_LOGI(TAG, "Real calibration: %f", systemDefaultValue.real_Cal);
  ESP_LOGI(TAG, "Image calibration: %f", systemDefaultValue.image_Cal);
  ESP_LOGI(TAG, "Log level: %d", systemDefaultValue.logLevel);
  modbusLoadCalibFromEeprom();
  dataSyncUnlockSystemConfig();
  return true;
}

void setupModbusAgentForexternal485(){
  //address는 항상 1이다.
  uint8_t address_485 = get485Address();
  ESP_LOGI(TAG, "Address_485: %d", address_485);
  //external485.useStopControll =0;
  Serial1.begin(9600, SERIAL_8N1, SERIAL_RX1, SERIAL_TX1);
  external485.begin(Serial1,9600,1,2000);
  external485.registerWorker(address_485,READ_COIL,&FC01);
  external485.registerWorker(address_485,READ_HOLD_REGISTER,&FC03);
  external485.registerWorker(address_485,READ_INPUT_REGISTER,&FC04);
  external485.registerWorker(address_485,WRITE_COIL,&FC05);
  external485.registerWorker(address_485,WRITE_HOLD_REGISTER,&FC06);

};

/* All Off will return 0  
*   else return value;
* Ret : 모든 릴레이의 합의 값이다.
*/
/* 셀을 선택한다. modbusId는 1부터 시작하며 설치되어 있는 배터리의 수보다 작아야 한다. 
* 
*/

const int  measuredImpedance_1[20]={
    267,265,292,255,271,
    274,383,307,277,272,
    262,267,294,278,270,
    285,259,289,262,254
  };
const int measuredImpedance_2[20]={
    334,337,349,340,345,
    334,331,350,337,339,
    334,332,349,337,328,
    343,341,358,330,334
  };
const int measuredVoltage_1[20]={
    1351,1321,1317,1311,1314,
    1320,1322,1320,1321,1320,
    1325,1339,1338,1338,1344,
    1353,1343,1359,1343,1352
  };
const int measuredVoltage_2[20]={
    1339,1340,1340,1339,1338,
    1335,1335,1336,1336,1335,
    1334,1334,1333,1334,1334,
    1334,1334,1332,1332,1333
  };
void initCellValue()
{
  if (systemDefaultValue.modbusId == 1)
  {
    for(int i=0;i<20;i++){
      cellvalue[i].impendance = float(measuredImpedance_1[i])/100.0f;
    }
  }
  else
  {
    for(int i=0;i<20;i++){
      cellvalue[i].impendance = float(measuredImpedance_2[i])/100.0f;
    }
  }
}

/**
 * 셀당 ADS1220 평균 샘플 수. 20 SPS·DRDY 대기 시 대략 (샘플 수)×50ms/셀 근처 → 전체 스캔 시간에 직결.
 * 16 + 순환필터는 노이즈 억제가 겹침; 빠른 스캔이면 샘플 수를 줄이고 스캔 간 순환필터로 보완하는 편이 낫다.
 */
#define ADS1220_SAMPLES_PER_CELL 4
#define ADS1220_DR_TIMEOUT_MS 500
#define ADS1220_INTER_SAMPLE_US 50

/** MUX·아날로그 안정화: 임피던스 측정 (ms). */
#define MUX_IMPEDANCE_SETTLE_MS 2000
/** 전압만 읽을 때: setOutput 후 RC·MUX 안정화 (ms). */
#define MUX_VOLTAGE_SETTLE_MS 1000

/** 전압: 3초 주기, 방문당 버스트 평균. */
#define CELL_VOLTAGE_INTERVAL_MS 3000
#define CELL_VOLT_BURST_SAMPLES 5
#define CELL_VOLT_BURST_GAP_MS 10

/** changeAD5940ToMeasurement(false) 직후 readImp() 전 (ms). */
#define AD5940_SETTLE_AFTER_OFF_MS 150

/** 임피던스: EEPROM ImpedanceMeasurePeriod(초), 0 → 기본 1시간. */

/** 1: 3초마다 전압+임피던스 함께(충전 테스트). 0: 전압 3초 / 임피던스 주기 분리. */
#define MEASURE_TEST_COMBINED_VZ 1
/** 1: Z 워밍업 매 회차 #01~#N 로그 (충전 모니터링). */
#define IMP_MONITOR_LOG_ALL 1

/** scanBatteriesAds1220 전압 스무딩용. */
#define CELL_MEAS_FILTER_DEPTH 2

/** 미장착·저전압 셀: 임피던스 무효 (V). */
#define CELL_VOLTAGE_IMP_VALID_MIN_V 0.6f

/** 측정 순환 셀 수. 0 = EEPROM installed_cells(현장 15 등), 양수 = 검증용 고정. */
#define MEASURE_ACTIVE_CELLS 0

static uint16_t measureActiveCellCount(void)
{
  uint16_t n;
  if (MEASURE_ACTIVE_CELLS > 0)
    n = (uint16_t)MEASURE_ACTIVE_CELLS;
  else
    n = systemDefaultValue.installed_cells;
  if (n < 1)
    n = 1;
  if (n > MAX_INSTALLED_CELLS)
    n = MAX_INSTALLED_CELLS;
  return n;
}

/** 미장착·무전압: 이 전압 미만이면 Z 측정 생략. */
static bool cellVoltageAllowsImpedanceV(float v)
{
  return v >= CELL_VOLTAGE_IMP_VALID_MIN_V;
}

static float s_cellVoltRing[MAX_INSTALLED_CELLS][CELL_MEAS_FILTER_DEPTH];
static uint8_t s_cellVoltRingIdx[MAX_INSTALLED_CELLS];
static uint8_t s_cellVoltRingCount[MAX_INSTALLED_CELLS];
static float s_cellVoltRingSum[MAX_INSTALLED_CELLS];

static float s_cellImpRing[MAX_INSTALLED_CELLS][CELL_MEAS_FILTER_DEPTH];
static uint8_t s_cellImpRingIdx[MAX_INSTALLED_CELLS];
static uint8_t s_cellImpRingCount[MAX_INSTALLED_CELLS];
static float s_cellImpRingSum[MAX_INSTALLED_CELLS];

static void resetCellMeasurementFilters(void)
{
  for (unsigned i = 0; i < MAX_INSTALLED_CELLS; i++)
  {
    s_cellVoltRingIdx[i] = 0;
    s_cellVoltRingCount[i] = 0;
    s_cellVoltRingSum[i] = 0.0f;
    s_cellImpRingIdx[i] = 0;
    s_cellImpRingCount[i] = 0;
    s_cellImpRingSum[i] = 0.0f;
    for (uint8_t k = 0; k < CELL_MEAS_FILTER_DEPTH; k++)
    {
      s_cellVoltRing[i][k] = 0.0f;
      s_cellImpRing[i][k] = 0.0f;
    }
  }
}

static float cellRingPush(float *ring, uint8_t *pIdx, uint8_t *pCount, float *pSum, float x)
{
  const uint8_t cap = (uint8_t)CELL_MEAS_FILTER_DEPTH;
  uint8_t i = *pIdx;
  if (*pCount < cap)
  {
    ring[i] = x;
    *pSum += x;
    (*pCount)++;
    *pIdx = (uint8_t)((i + 1u) % cap);
    return *pSum / (float)(*pCount);
  }
  *pSum -= ring[i];
  ring[i] = x;
  *pSum += x;
  *pIdx = (uint8_t)((i + 1u) % cap);
  return *pSum / (float)cap;
}

/** 외부 MUX 회로용 OLAT 값: 셀 0→0x01 … 셀 19→0x14(20). */
static uint8_t mcpBatteryMuxPattern(unsigned cellIndex)
{
  return (uint8_t)(cellIndex );
}

/**
 * installed_cells만큼 순차 스캔해 cellvalue[]에 필터된 전압·임피던스 반영.
 * AD5940과 SPI 버스를 공유하므로 동시 접근 시 충돌 가능 — 필요하면 뮤텍스·순서 조정.
 */
void scanBatteriesAds1220(uint8_t nCells=MAX_INSTALLED_CELLS)
{
  const uint32_t t0 = millis();

  for (unsigned i = 0; i < nCells; i++)
  {
    Mcp23s08_setOutput(mcpBatteryMuxPattern(i+1));
    delay(5); /* 멀티플렉서·아날로그 안정화 */

    const float vSample = Ads1220_readAveragedVoltageOnChannel(
        0, ADS1220_SAMPLES_PER_CELL, ADS1220_DR_TIMEOUT_MS, ADS1220_INTER_SAMPLE_US);
    cellvalue[i].voltage = cellRingPush(
        s_cellVoltRing[i], &s_cellVoltRingIdx[i], &s_cellVoltRingCount[i], &s_cellVoltRingSum[i], vSample);
    /* 임피던스는 AD5940 실측만 링에 넣음 — initCellValue 더미값이 섞이면 1회차 Z가 깨짐 */
  }
  ESP_LOGI(TAG, "ADS1220 cell scan: %u cells, %lums", (unsigned)nCells, (unsigned long)(millis() - t0));
}
// 인터럽트 서비스 루틴 (ISR)
// void IRAM_ATTR handleInterrupt() {
//   // 인터럽트가 발생했을 때 실행될 코드
//   isAd5940Interrupt = true;
// }
void AD5940_();

uint8_t get485Address()
{
  int address1 = digitalRead(RS_485ADD1);
  int address2 = digitalRead(RS_485ADD2);
  uint8_t address = address1 << 1 | address2;
  return address;
}
/** MUX 안정화 후 ADS1220 단일 변환 전압(V). */
static float readCellVoltageOnce(void)
{
  return Ads1220_readAveragedVoltageOnChannel(
      0, 1, ADS1220_DR_TIMEOUT_MS, 0);
}

static uint32_t impedanceMeasurePeriodMs(void)
{
  dataSyncLockSystemConfig();
  uint32_t sec = (uint32_t)systemDefaultValue.ImpedanceMeasurePeriod;
  dataSyncUnlockSystemConfig();
  if (sec == 0)
    sec = IMP_MEASURE_PERIOD_DEFAULT_SEC;
  return sec * 1000UL;
}

static uint16_t impedanceReadMax(void)
{
  dataSyncLockSystemConfig();
  uint16_t n = systemDefaultValue.ACVoltPP;
  dataSyncUnlockSystemConfig();
  if (n == 0 || n > IMP_READ_MAX_MAX)
    n = (uint16_t)IMP_READ_MAX_DEFAULT;
  return n;
}

static uint8_t impedanceStableWindow(uint16_t readMax)
{
  dataSyncLockSystemConfig();
  uint16_t n = systemDefaultValue.DCVolt;
  dataSyncUnlockSystemConfig();
  if (n == 0 || n > IMP_STABLE_WINDOW_MAX)
    n = (uint16_t)IMP_STABLE_WINDOW_DEFAULT;
  if (n > readMax)
    n = readMax;
  if (n < 2)
    n = 2;
  return (uint8_t)n;
}

static float impedanceStableToleranceRatio(void)
{
  dataSyncLockSystemConfig();
  uint8_t pct = systemDefaultValue.VoltageFactor;
  dataSyncUnlockSystemConfig();
  if (pct == 0 || pct > 20)
    pct = (uint8_t)IMP_STABLE_REL_TOL_DEFAULT_PERCENT;
  return (float)pct / 100.0f;
}

static uint8_t impedancePostStableSamples(void)
{
  dataSyncLockSystemConfig();
  uint8_t n = systemDefaultValue.TemperatureFactor;
  dataSyncUnlockSystemConfig();
  if (n == 0 || n > IMP_POST_STABLE_SAMPLES_MAX)
    n = (uint8_t)IMP_POST_STABLE_SAMPLES_DEFAULT;
  return n;
}

static float impedanceMinValidMohm(void)
{
  dataSyncLockSystemConfig();
  uint16_t deci = systemDefaultValue.RcalLoopCount;
  dataSyncUnlockSystemConfig();
  if (deci == 0 || deci > 10000)
    deci = (uint16_t)IMP_MAG_MIN_VALID_MOHM_DEFAULT_DECI;
  return (float)deci / 10.0f;
}

static bool impSampleUsable(const fImpCar_Type *car)
{
  if (car == NULL)
    return false;
  const float mag = AD5940_ComplexMag((fImpCar_Type *)car);
  return (car->Real > 0.0f) && (mag >= impedanceMinValidMohm());
}

/** EEPROM baseImpendance[] 인코딩: mOhm × 100 (Modbus FC04 80~95). */
static int16_t impMohmToEepromCenti(float z_mOhm)
{
  if (z_mOhm <= 0.0f)
    return 0;
  const int32_t v = (int32_t)(z_mOhm * 100.0f + 0.5f);
  if (v > 32767)
    return 32767;
  return (int16_t)v;
}

static float impEepromCentiToMohm(int16_t centi)
{
  if (centi <= 0)
    return 0.0f;
  return (float)centi / 100.0f;
}

static void applyCellImpedanceFromEeprom(unsigned batNo)
{
  const uint16_t nActive = measureActiveCellCount();
  if (batNo < 1 || batNo > (int)nActive)
    return;
  const unsigned idx = (unsigned)(batNo - 1);
  dataSyncLockSystemConfig();
  const int16_t baseCenti = systemDefaultValue.baseImpendance[idx];
  dataSyncUnlockSystemConfig();
  const float z = impEepromCentiToMohm(baseCenti);
  if (z <= 0.0f)
    return;
  cellvalue[idx].impendance = z;
  cellvalue[idx].baseImpendance = baseCenti;
}

static void applyAllCellImpedanceFromEeprom(void)
{
  const uint16_t n = measureActiveCellCount();
  for (uint16_t c = 1; c <= n; c++)
    applyCellImpedanceFromEeprom((int)c);
}

/** oldC 대비 newC 변화량이 EEPROM 설정(%) 이상인지 (정수 centi-mOhm). */
static bool impEepromChangeEnough(int16_t oldC, int16_t newC, float minRatio)
{
  if (oldC <= 0)
    return true;
  if (newC == oldC)
    return false;
  const int64_t delta = (int64_t)newC - (int64_t)oldC;
  const int64_t absDelta = delta < 0 ? -delta : delta;
  const int64_t minDelta = (int64_t)((float)oldC * minRatio + 0.5f);
  return absDelta >= minDelta;
}

/** 유효 Z 측정 성공 시: 기존 EEPROM과 설정값(%) 이상 다를 때만 저장. */
static bool tryPersistCellImpedanceToEeprom(unsigned batNo, float z_mOhm)
{
  const unsigned idx = (unsigned)(batNo - 1);
  const int16_t newC = impMohmToEepromCenti(z_mOhm);
  dataSyncLockSystemConfig();
  const int16_t oldC = systemDefaultValue.baseImpendance[idx];
  uint8_t minPctCfg = systemDefaultValue.ImpedanceFactor;
  if (minPctCfg == 0 || minPctCfg > 100)
    minPctCfg = (uint8_t)IMP_EEPROM_CHANGE_DEFAULT_PERCENT;
  const float minRatio = (float)minPctCfg / 100.0f;

  if (newC <= 0)
  {
    dataSyncUnlockSystemConfig();
    return false;
  }

  if (oldC > 0 && !impEepromChangeEnough(oldC, newC, minRatio))
  {
    const float oldM = impEepromCentiToMohm(oldC);
    const float pct = oldM > 0.0f ? ((z_mOhm - oldM) / oldM) * 100.0f : 0.0f;
    const float minPct = (float)minPctCfg;
    if (s_espLogEnabled)
      ESP_LOGI(TAG,
               "cell %u Z EEPROM keep %.2f mOhm (new %.2f, %+.1f%% < ±%.0f%%)",
               (unsigned)batNo, oldM, z_mOhm, pct, minPct);
    dataSyncUnlockSystemConfig();
    return false;
  }

  systemDefaultValue.baseImpendance[idx] = newC;
  (void)readnWriteEEProm(true);
  dataSyncUnlockSystemConfig();
  cellvalue[idx].baseImpendance = newC;
  if (s_espLogEnabled)
    ESP_LOGI(TAG, "cell %u Z EEPROM saved %.2f mOhm (was %.2f mOhm)",
             (unsigned)batNo, z_mOhm, oldC > 0 ? impEepromCentiToMohm(oldC) : 0.0f);
  return true;
}

/** 연속 5샘플 창 [startIdx .. startIdx+4]: 첫·끝 상대오차 + max-min. */
static bool impWindowIsStable(fImpCar_Type *samples, int startIdx, uint8_t stableWindow)
{
  const float minMag = impedanceMinValidMohm();
  const float tolRatio = impedanceStableToleranceRatio();
  for (uint8_t k = 0; k < stableWindow; k++)
  {
    if (!impSampleUsable(&samples[startIdx + k]))
      return false;
  }

  const float z0 = AD5940_ComplexMag(&samples[startIdx]);
  const float z4 = AD5940_ComplexMag(&samples[startIdx + stableWindow - 1]);
  const float denom = fmaxf(z0, z4);
  if (denom < minMag)
    return false;
  if (fabsf(z4 - z0) / denom > tolRatio)
    return false;

  float mn = z0;
  float mx = z0;
  for (uint8_t k = 0; k < stableWindow; k++)
  {
    const float z = AD5940_ComplexMag(&samples[startIdx + k]);
    if (z < mn)
      mn = z;
    if (z > mx)
      mx = z;
  }
  return ((mx - mn) / denom) <= tolRatio;
}

/**
 * 셀 전압 측정(ADS1220 버스트 평균 → 순환 필터 → cellvalue).
 * @return 필터 적용 후 전압(V). 범위 밖 batNo면 0.
 */
static float readCellVoltageForBat(int batNo)
{
  const uint16_t nActive = measureActiveCellCount();
  if (batNo < 1 || batNo > (int)nActive)
    return 0.0f;
  const unsigned idx = (unsigned)(batNo - 1);

  changeAD5940ToMeasurement(true);
  Mcp23s08_setOutput(mcpBatteryMuxPattern((unsigned)batNo));
  delay(MUX_VOLTAGE_SETTLE_MS);

  float vSum = 0.0f;
  for (uint8_t s = 0; s < (uint8_t)CELL_VOLT_BURST_SAMPLES; s++)
  {
    if (s > 0)
      delay(CELL_VOLT_BURST_GAP_MS);
    vSum += readCellVoltageOnce();
  }
  const float vAvg = vSum / (float)CELL_VOLT_BURST_SAMPLES;
  changeAD5940ToMeasurement(false);

  const float vFiltered = cellRingPush(
      s_cellVoltRing[idx], &s_cellVoltRingIdx[idx], &s_cellVoltRingCount[idx],
      &s_cellVoltRingSum[idx], vAvg);
  cellvalue[idx].voltage = vFiltered;

  if (s_espLogEnabled)
    ESP_LOGI(TAG, "cell %u V=%.4f (3s)", (unsigned)batNo, vFiltered);
  return vFiltered;
}

/**
 * 임피던스: 최대 IMP_READ_MAX회, 5샘플 창 안정 후 5회 추가 평균.
 * @return true면 *outZ에 mΩ 저장; false면 이전값 유지
 */
static bool readCellImpedanceWithWarmup(int batNo, float *outZ)
{
  const uint16_t nActive = measureActiveCellCount();
  if (batNo < 1 || batNo > (int)nActive || outZ == NULL)
    return false;
  const unsigned idx = (unsigned)(batNo - 1);
  const float prevZ = cellvalue[idx].impendance;

  if (!cellVoltageAllowsImpedanceV(cellvalue[idx].voltage))
  {
    cellvalue[idx].impendance = 0.0f;
    *outZ = 0.0f;
    if (s_espLogEnabled)
      ESP_LOGI(TAG, "cell %u Z skipped — no battery (V=%.4f < %.2f V)",
               (unsigned)batNo, cellvalue[idx].voltage, CELL_VOLTAGE_IMP_VALID_MIN_V);
    return false;
  }

  changeAD5940ToMeasurement(false);
  Mcp23s08_setOutput(mcpBatteryMuxPattern((unsigned)batNo));
  delay(MUX_IMPEDANCE_SETTLE_MS);
  delay(AD5940_SETTLE_AFTER_OFF_MS);

  fImpCar_Type samples[IMP_READ_MAX_MAX + IMP_POST_STABLE_SAMPLES_MAX];
  int nSamples = 0;
  const uint8_t postStableSamples = impedancePostStableSamples();
  const uint16_t readMax = impedanceReadMax();
  const uint8_t stableWindow = impedanceStableWindow(readMax);

#if IMP_MONITOR_LOG_ALL
  if (s_espLogEnabled)
    ESP_LOGI(TAG, "cell %u Z monitor start (max %u reads, window %u)",
             (unsigned)batNo, (unsigned)readMax, (unsigned)stableWindow);
#endif

  for (uint16_t i = 0; i < readMax; i++)
  {
    fImpCar_Type car;
    const float mag = AD5940_readImpMagnitude(&car);
#if IMP_MONITOR_LOG_ALL
    if (s_espLogEnabled)
      ESP_LOGI(TAG, "  cell %u Z #%03d: %.3f mOhm (real=%.1f image=%.1f)",
               (unsigned)batNo, i + 1, mag, car.Real, car.Image);
#endif
    if (!impSampleUsable(&car))
    {
#if !IMP_MONITOR_LOG_ALL
      ESP_LOGD(TAG, "cell %u Z #%d: skip (mag=%.3f real=%.3f)",
               (unsigned)batNo, i + 1, mag, car.Real);
#endif
      continue;
    }
    samples[nSamples++] = car;

    if (nSamples >= (int)stableWindow)
    {
      const int winStart = nSamples - (int)stableWindow;
      if (!impWindowIsStable(samples, winStart, stableWindow))
        continue;

      float sum = 0.0f;
      int postCount = 0;
      for (uint8_t p = 0; p < postStableSamples; p++)
      {
        fImpCar_Type postCar;
        const float postMag = AD5940_readImpMagnitude(&postCar);
#if IMP_MONITOR_LOG_ALL
        if (s_espLogEnabled)
          ESP_LOGI(TAG, "  cell %u Z post #%d: %.3f mOhm", (unsigned)batNo, postCount + 1, postMag);
#endif
        if (!impSampleUsable(&postCar))
          continue;
        sum += postMag;
        postCount++;
      }

      if (postCount > 0)
      {
        *outZ = sum / (float)postCount;
        cellvalue[idx].impendance = cellRingPush(
            s_cellImpRing[idx], &s_cellImpRingIdx[idx], &s_cellImpRingCount[idx],
            &s_cellImpRingSum[idx], *outZ);

        struct timeval tmv;
        gettimeofday(&tmv, NULL);
        cellvalue[idx].readTime = tmv.tv_sec;
        cellvalue[idx].voltageCompensation = systemDefaultValue.voltageCompensation[idx];
        cellvalue[idx].impendanceCompensation = systemDefaultValue.impendanceCompensation[idx];
        cellvalue[idx].baseVoltage = systemDefaultValue.baseVoltage[idx];
        cellvalue[idx].baseImpendance = systemDefaultValue.baseImpendance[idx];

        if (s_espLogEnabled)
          ESP_LOGI(TAG,
                   "cell %u Z=%.3f mOhm valid (3%% stable@%d +%d avg, reads=%d)",
                   (unsigned)batNo, cellvalue[idx].impendance, winStart + stableWindow,
                   postCount, i + 1 + postCount);
        tryPersistCellImpedanceToEeprom((unsigned)batNo, cellvalue[idx].impendance);
        return true;
      }
    }
  }

  applyCellImpedanceFromEeprom((unsigned)batNo);
  *outZ = cellvalue[idx].impendance;
  if (*outZ <= 0.0f)
    *outZ = prevZ;

  if (s_espLogEnabled)
    ESP_LOGW(TAG,
             "cell %u Z: not stable in %d reads (likely charging) — using EEPROM %.3f mOhm",
             (unsigned)batNo, (int)readMax, *outZ);
  return false;
}

/** Modbus FC06 주소 50 — 기준 저항: 5% 조건 없이 EEPROM 저장. */
static void forcePersistCellImpedanceToEeprom(unsigned batNo, float z_mOhm)
{
  const unsigned idx = (unsigned)(batNo - 1);
  const int16_t newC = impMohmToEepromCenti(z_mOhm);
  if (newC <= 0)
    return;
  dataSyncLockSystemConfig();
  systemDefaultValue.baseImpendance[idx] = newC;
  (void)readnWriteEEProm(true);
  dataSyncUnlockSystemConfig();
  cellvalue[idx].baseImpendance = newC;
  if (s_espLogEnabled)
    ESP_LOGI(TAG, "cell %u Z baseline EEPROM %.2f mOhm", (unsigned)batNo, z_mOhm);
}

static bool s_baselineScanRunCell = false;

void modbusOnFc06Reg50Write(uint16_t value)
{
  if (value == 0)
  {
    modbusReg50BaseImpProgress = 0;
    s_baselineScanRunCell = false;
    return;
  }
  if (value == 1 && modbusReg50BaseImpProgress == 0)
  {
    modbusReg50BaseImpProgress = 1;
    s_baselineScanRunCell = true;
    if (s_espLogEnabled)
      ESP_LOGI(TAG, "Modbus baseline Z scan start");
  }
}

void modbusBaselineScanPoll(void)
{
  if (!s_baselineScanRunCell || modbusReg50BaseImpProgress == 0)
    return;

  const uint16_t n = measureActiveCellCount();
  const int bat = (int)modbusReg50BaseImpProgress;
  if (bat < 1 || bat > (int)n)
  {
    modbusReg50BaseImpProgress = 0;
    s_baselineScanRunCell = false;
    return;
  }

  s_baselineScanRunCell = false;
  readCellVoltageForBat(bat);
  float z = 0.0f;
  if (cellVoltageAllowsImpedanceV(cellvalue[(unsigned)(bat - 1)].voltage))
    readCellImpedanceWithWarmup(bat, &z);
  if (z > 0.0f)
    forcePersistCellImpedanceToEeprom((unsigned)bat, z);

  if ((unsigned)bat >= n)
  {
    modbusReg50BaseImpProgress = 0;
    if (s_espLogEnabled)
      ESP_LOGI(TAG, "Modbus baseline Z scan complete (%u cells)", (unsigned)n);
    return;
  }

  modbusReg50BaseImpProgress = (uint16_t)(bat + 1);
  s_baselineScanRunCell = true;
}

static unsigned long now;
static unsigned long previousVoltageMs = 0;
static unsigned long lastImpedancePeriodMs = 0;
static unsigned long lastNtcReadMs = 0;
static unsigned long lastWifiWatchMs = 0;
/** IN_TH1/IN_TH2 NTC 갱신 주기 */
static const unsigned long NTC_READ_INTERVAL_MS = 2000;
static const unsigned long WIFI_WATCH_INTERVAL_MS = 5000;
static bool s_impedanceSessionActive = false;
static uint16_t s_impedanceSessionCell = 1;
static uint16_t voltageRotateBatNo = 1;

void setup()
{

  dataSyncInit();
  EEPROM.begin((unsigned)EEPROM_NV_RESERVED_BYTES);
  (void)readnWriteEEProm(false);
  pinsetup();
  ntcTemperatureInit();
  ntcTemperatureUpdate();
  ctCurrentInit();
  ctCurrentUpdate();
  ESP_LOGI(TAG, "NTC TH1=%.1f C  TH2=%.1f C",
           ntcTemperatureC_x10[0] / 10.0f, ntcTemperatureC_x10[1] / 10.0f);
  ESP_LOGI(TAG, "CT current=%.1f A (AIN2=%.4f V)",
           ctCurrentGetAmps(), packCurrentAin2Volts);
  // AD5940 인터럽트는 AD5940_MCUResourceInit()에서 Ext_Int0_Handler로 등록됨
  Serial.begin(115200);

  String strResetReason = "System booting reason is  ";
  bool dataReload = false;
  Serial.println("Flash Memory Init....Waiting....");
  lsFile.littleFsInitFast(0);

  String bleName = "TIMP_";
  String WifiAddress = WiFi.macAddress();
  bleName += WifiAddress;
  bleName += "_";
  bleName += systemDefaultValue.modbusId;
  SerialBT.begin(bleName.c_str());
  installBtLogMirror();
  Serial.printf("\nBluetooth Name : %s\n",bleName.c_str());
  wifiApmodeConfig();
  printWebLoginCredentials();
#ifdef WIFI_AP_MODE
  if (s_serviceTaskHandle == nullptr)
  {
    xTaskCreatePinnedToCore(serviceTask, "ServiceTask", 6144, NULL, 1, &s_serviceTaskHandle, 0);
    ESP_LOGI(TAG, "ServiceTask started (BT+WEB, core0, prio1)");
  }
#endif

  lsFile.writeLogString(strResetReason);

  SPI.setFrequency(spiClk);
  SPI.begin(SCK, MISO, MOSI, CS_5940);
  pinMode(SS, OUTPUT); // VSPI SS -> 아니다..이것은 리셋용이다.

  Mcp23s08_begin(A23S08_CS, CS_5940, ADS1220_CS, (uint32_t)spiClk);
  ESP_LOGI(TAG, "MCP23S08 GP walk 테스트 (20회, 500ms)");
  Mcp23s08_testPortWalk(1, 100);
  Mcp23s08_end(); /* MCP·CS_5940·ADS1220_CS 비선택 */
  ESP_LOGI(TAG, "MCP23S08 테스트 종료");

  /* ADS1220: AIN0–AVSS, SPI Mode1 (라이브러리). 필요 없으면 이 블록만 제거 */
  Ads1220_begin(ADS1220_CS, ADS1220_DR, CS_5940, A23S08_CS, (uint32_t)spiClk);
  Ads1220_reset();
  Mcp23s08_initOutputsAll();
  initCellValue();
  applyAllCellImpedanceFromEeprom();
  dataSyncPublishCellSnapshot(cellvalue, MAX_INSTALLED_CELLS);
  Mcp23s08_setOutput(mcpBatteryMuxPattern(0));
  //for(int i=0;i<1;i){
  scanBatteriesAds1220(1);

  void AD5940_init();
  void AD5940_ShutDown();
  // void AD5940_DriveCE0Low_NoLoopback();
  // void AD5940_DriveCE0Low_WithLoopback();
  // void AD5940_OutputSineOnCE0(float freqHz, float offsetMv, float amplitudeMvpp, bool withLoopback);
  AD5940_init();
  // AD5940_DriveCE0Low_NoLoopback();   // Test 1: CE0 low drive, AIN1 loopback off
  // AD5940_DriveCE0Low_WithLoopback(); // Test 2: CE0 low drive, AIN1 loopback on
  // AD5940_OutputSineOnCE0(1000.0f, 200.0f, 100.0f, false); // Test 3: CE0 sine, 0.6V offset, no loopback
  

  float real , image;
  float ImpMagnitude = AD5940_calibration(&real,&image);
  resetCellMeasurementFilters();

  //AD5940_ShutDown();

  Mcp23s08_setOutput(mcpBatteryMuxPattern(1));

  simpleCli.outputStream = &Serial;
  vTaskDelay(1000);
  ESP_LOGI(TAG, "System Started at %s mode", systemDefaultValue.runMode == 0 ? "Manual" : "Auto");
  ESP_LOGI(TAG, "\nEEPROM installed Bat number %d", systemDefaultValue.installed_cells);
  ESP_LOGI(TAG, "Active measure cells: %u (MEASURE_ACTIVE_CELLS=%d)",
           (unsigned)measureActiveCellCount(), MEASURE_ACTIVE_CELLS);
#if MEASURE_TEST_COMBINED_VZ
  ESP_LOGI(TAG, "TEST: every %ums V+Z together, Z max %u reads (charger monitor)",
           (unsigned)CELL_VOLTAGE_INTERVAL_MS, (unsigned)impedanceReadMax());
#else
  ESP_LOGI(TAG, "Voltage interval %ums, impedance period %lus (EEPROM ImpedanceMeasurePeriod)",
           (unsigned)CELL_VOLTAGE_INTERVAL_MS,
           (unsigned long)(impedanceMeasurePeriodMs() / 1000UL));
#endif
  lastImpedancePeriodMs = millis();
  previousVoltageMs = millis();
  lastNtcReadMs = millis();
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
#ifdef WEBOTA
  xTaskCreate(NetworkTask, "NetworkTask", 5000, NULL, 1, &h_networkTask); // PCB 패턴문제로 사용하지 않는다.
#endif

  setupModbusAgentForexternal485();
  //xTaskCreate(AD5940_Main, "AD5940_Main", 5000, NULL, 1, NULL);
  // esp_log_level_t level;
  // switch (systemDefaultValue.logLevel)
  // {
  // case 0:
  //   level = ESP_LOG_NONE;
  //   break;
  // case 1:
  //   ESP_LOG_ERROR;
  //   break;
  // case 2:
  //   ESP_LOG_WARN;
  //   break;
  // case 3:
  //   ESP_LOG_INFO;
  //   break;
  // case 4:
  //   ESP_LOG_DEBUG;
  //   break;
  // case 5:
  //   ESP_LOG_VERBOSE;
  //   break;
  // }
  //esp_log_level_set("*", level);
};

void loop(void)
{
  if (s_lastEspLogEnabledApplied != s_espLogEnabled)
  {
    esp_log_level_set("*", s_espLogEnabled ? ESP_LOG_INFO : ESP_LOG_NONE);
    s_lastEspLogEnabledApplied = s_espLogEnabled;
  }
  now = millis();
  esp_task_wdt_reset();

  if (modbusBaselineScanIsActive())
  {
    modbusBaselineScanPoll();
    if ((now - lastNtcReadMs) >= NTC_READ_INTERVAL_MS)
    {
      lastNtcReadMs = now;
      ntcTemperatureUpdate();
      ctCurrentUpdate();
    }
#ifdef WIFI_AP_MODE
    if (s_serviceTaskHandle == nullptr)
      restApiHandle();
#endif
    dataSyncPublishCellSnapshot(cellvalue, MAX_INSTALLED_CELLS);
    vTaskDelay(100);
    return;
  }

#if MEASURE_TEST_COMBINED_VZ
  if ((now - previousVoltageMs) >= (unsigned long)CELL_VOLTAGE_INTERVAL_MS)
  {
    const int bat = (int)voltageRotateBatNo;
    const unsigned idx = (unsigned)(bat - 1);
    const float v = readCellVoltageForBat(bat);
    const bool hasBat = cellVoltageAllowsImpedanceV(v);
    if (hasBat)
    {
      float zDummy = 0.0f;
      readCellImpedanceWithWarmup(bat, &zDummy);
      previousVoltageMs = now;
    }
    else
    {
      cellvalue[idx].impendance = 0.0f;
      /* 무전압: 3초 대기 없이 다음 셀 */
      previousVoltageMs = 0;
    }
    voltageRotateBatNo++;
    if (voltageRotateBatNo > measureActiveCellCount())
      voltageRotateBatNo = 1;
    esp_task_wdt_reset();
  }
#else
  if (s_impedanceSessionActive)
  {
    const int bat = (int)s_impedanceSessionCell;
    const float v = readCellVoltageForBat(bat);
    float zDummy = 0.0f;
    if (cellVoltageAllowsImpedanceV(v))
      readCellImpedanceWithWarmup(bat, &zDummy);
    s_impedanceSessionCell++;
    if (s_impedanceSessionCell > measureActiveCellCount())
    {
      s_impedanceSessionActive = false;
      ESP_LOGI(TAG, "impedance round complete (%u cells)", (unsigned)measureActiveCellCount());
    }
  }
  else
  {
    const uint32_t impPeriodMs = impedanceMeasurePeriodMs();
    if ((now - lastImpedancePeriodMs) >= impPeriodMs)
    {
      lastImpedancePeriodMs = now;
      s_impedanceSessionActive = true;
      s_impedanceSessionCell = 1;
      ESP_LOGI(TAG, "impedance session start (period %lu s)", (unsigned long)(impPeriodMs / 1000UL));
    }

    if ((now - previousVoltageMs) >= (unsigned long)CELL_VOLTAGE_INTERVAL_MS)
    {
      const int bat = (int)voltageRotateBatNo;
      const unsigned idx = (unsigned)(bat - 1);
      const float v = readCellVoltageForBat(bat);
      if (cellVoltageAllowsImpedanceV(v))
        previousVoltageMs = now;
      else
      {
        cellvalue[idx].impendance = 0.0f;
        previousVoltageMs = 0;
      }
      voltageRotateBatNo++;
      if (voltageRotateBatNo > measureActiveCellCount())
        voltageRotateBatNo = 1;
    }
  }
#endif

  if ((now - lastNtcReadMs) >= NTC_READ_INTERVAL_MS)
  {
    lastNtcReadMs = now;
    ntcTemperatureUpdate();
    ctCurrentUpdate();
    if (s_espLogEnabled)
      ESP_LOGI(TAG, "NTC TH1=%.1f C  TH2=%.1f C  CT=%.1f A (AIN2=%.4f V)",
               ntcTemperatureC_x10[0] / 10.0f, ntcTemperatureC_x10[1] / 10.0f,
               ctCurrentGetAmps(), packCurrentAin2Volts);
  }

#ifdef WIFI_AP_MODE
  if (s_apRestartRequested)
  {
    s_apRestartRequested = false;
    ESP_LOGW(TAG, "WiFi AP restart by event");
    WiFi.softAPdisconnect(true);
    startApOnly();
  }

  if ((now - lastWifiWatchMs) >= WIFI_WATCH_INTERVAL_MS)
  {
    lastWifiWatchMs = now;
    const IPAddress apIp = WiFi.softAPIP();
    const bool apModeOk = (WiFi.getMode() == WIFI_MODE_AP) || (WiFi.getMode() == WIFI_MODE_APSTA);
    const bool apIpOk = (apIp[0] == 192 && apIp[1] == 168 && apIp[2] == 11 && apIp[3] == 1);
    if (!apModeOk || !apIpOk)
    {
      ESP_LOGW(TAG, "WiFi AP watchdog recover (mode=%d ip=%s)",
               (int)WiFi.getMode(), apIp.toString().c_str());
      WiFi.softAPdisconnect(true);
      startApOnly();
    }
  }
#endif

#ifdef WIFI_AP_MODE
  if (s_serviceTaskHandle == nullptr)
    restApiHandle();
#endif

  dataSyncPublishCellSnapshot(cellvalue, MAX_INSTALLED_CELLS);
  vTaskDelay(100);
}