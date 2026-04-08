#include <Arduino.h>
#include <SPI.h>
#include <EEPROM.h>
#include <wifi.h>
#include <driver/adc.h>
#include "filesystem.h"
#include "mainGrobal.h"
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
#include "modbusCellModule.h"
#include "modbusLcdModule.h"
#include <Mcp23s08.h>
#include <Ads1220.h>

// #include <esp_int_wdt.h>
// #include <esp_task.h>
#include <esp_task_wdt.h>

#define MAIN_POWEROFF HIGH
#define MAIN_POWERON LOW 
#define WDT_TIMEOUT 100 
// 기본 vSPI와 일치한다
#define VSPI_MISO   MISO  // IO19
#define VSPI_MOSI   MOSI  // IO 23
#define VSPI_SCLK   SCK   // IO 18
#define VSPI_SS     15    // IO 15

#define NUM_VALUES 21


//SPIClass SPI;
static const int spiClk = 1000000; // 1 MHz
static char TAG[] ="Main";

TaskHandle_t *h_pxblueToothTask;
TaskHandle_t *h_pxNetworkTask;
nvsSystemSet systemDefaultValue;

ModbusServerRTU external485(2000,EXT_485EN_1);

uint32_t request_time;
uint16_t values[2];
uint16_t cellModbusIdReceived;
ExtendSerial extendSerial;

uint8_t selecectedCellNumber =0;
volatile bool isAd5940Interrupt = false;

_cell_value cellvalue[MAX_INSTALLED_CELLS];

extern SimpleCLI simpleCli;
uint16_t startBatnumber=1;

BluetoothSerial SerialBT;

BatDeviceInterface batDevice;


//void AD5940_ShutDown();

void pinsetup()
{
    pinMode(READ_BATVOL, INPUT);
    pinMode(AD5940_ISR, INPUT);

    pinMode(EXT_485EN_1, OUTPUT);
    digitalWrite(EXT_485EN_1, LOW);
    pinMode(RST_5940, OUTPUT);
    digitalWrite(RST_5940, HIGH);

    pinMode(RS_485ADD1, OUTPUT);
    digitalWrite(RST_5940, HIGH);
    pinMode(RS_485ADD2, OUTPUT);
    digitalWrite(RST_5940, HIGH);

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

void wifiApmodeConfig()
{
}
void readnWriteEEProm()
{
  uint8_t ipaddr1;
  if (EEPROM.read(0) != 0x55)
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
    systemDefaultValue.installed_cells= 20;
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
    EEPROM.writeByte(0, 0x55);
    EEPROM.writeBytes(1, (const byte *)&systemDefaultValue, sizeof(nvsSystemSet));
    EEPROM.commit();
  }
  EEPROM.readBytes(1, (byte *)&systemDefaultValue, sizeof(nvsSystemSet));
  if(systemDefaultValue.startBatnumber > systemDefaultValue.installed_cells  )
    systemDefaultValue.startBatnumber = systemDefaultValue.installed_cells;
  if(systemDefaultValue.startBatnumber == 0) 
  startBatnumber = 1;
}

void setupModbusAgentForexternal485(){
  //address는 항상 1이다.
  uint8_t address_485 = systemDefaultValue.modbusId; 

  //external485.useStopControll =0;
  external485.begin(Serial1,115200,1,2000);
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
// 인터럽트 서비스 루틴 (ISR)
// void IRAM_ATTR handleInterrupt() {
//   // 인터럽트가 발생했을 때 실행될 코드
//   isAd5940Interrupt = true;
// }
void AD5940_();

void setup()
{

  EEPROM.begin(sizeof(nvsSystemSet) + 1);
  readnWriteEEProm();
  pinsetup();
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
  Serial.printf("\nBluetooth Name : %s\n",bleName.c_str());
  wifiApmodeConfig();

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
  for(;;){
    long startTime = millis();
    const int32_t raw = Ads1220_readAveragedRawOnChannel(0, 16, 500, 100);

    ESP_LOGI(TAG, "ADS1220 AIN0 avgRaw=%ld (~%.4fV ) %ldms", (long)raw,
            (double)Ads1220_rawToVolts(raw, 2.048f, 1,7.506), millis() - startTime);
    delay(100);
  }
  Ads1220_end();

  simpleCli.outputStream = &Serial;
  vTaskDelay(1000);
  ESP_LOGI(TAG, "System Started at %s mode", systemDefaultValue.runMode == 0 ? "Manual" : "Auto");
  ESP_LOGI(TAG, "\nEEPROM installed Bat number %d", systemDefaultValue.installed_cells);
  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
#ifdef WEBOTA
  xTaskCreate(NetworkTask, "NetworkTask", 5000, NULL, 1, h_pxNetworkTask); // PCB 패턴문제로 사용하지 않는다.
#endif

  xTaskCreate(blueToothTask, "blueToothTask", 5000, NULL, 1, h_pxblueToothTask);
  xTaskCreate(AD5940_Main, "AD5940_Main", 5000, NULL, 1, NULL);
  esp_log_level_t level;
  switch (systemDefaultValue.logLevel)
  {
  case 0:
    level = ESP_LOG_NONE;
    break;
  case 1:
    ESP_LOG_ERROR;
    break;
  case 2:
    ESP_LOG_WARN;
    break;
  case 3:
    ESP_LOG_INFO;
    break;
  case 4:
    ESP_LOG_DEBUG;
    break;
  case 5:
    ESP_LOG_VERBOSE;
    break;
  }
  esp_log_level_set("*", level);
  for(int i=0;i<40;i++)cellvalue[i].impendance = 0.0;
};
static unsigned long previousSecondmills = 0;
static int everySecondInterval = 1000;

static int Interval_3Second = 3000;
static unsigned long previous_3Secondmills = 0;

static int Interval_5Second = 5000;
static unsigned long previous_5Secondmills = 0;

static int Interval_30Second = 30000;
static unsigned long previous_30Secondmills = 0;

static int Interval_60Second = 60000;
static unsigned long previous_60Secondmills = 0;

static unsigned long now;
//각각의 시간은 병렬로 수행된다.

//uint8_t globalModbusId =1;
uint8_t impedanceCellPosition=1;
static timeval tmv;
int16_t logForHour=0;
uint32_t loopCount=0;
static bool isModuleBootingOK=false;
static long elaspTime=-1;
int toggle=0;
void loop(void)
{
  bool bRet;
  void *parameters;
  esp_log_level_set("*",ESP_LOG_INFO);
  parameters = simpleCli.outputStream;
  now = millis(); 

  esp_task_wdt_reset();
  if ((now - previousSecondmills > everySecondInterval))
  {
    elaspTime++;
    toggle = toggle == 0 ? 1:0;
    if( elaspTime%10 ==0 )
      simpleCli.outputStream->printf("\nTime elasped : %d",elaspTime);
    previousSecondmills = now;
  }
  if ((now - previous_3Secondmills > Interval_3Second))
  {
    previous_3Secondmills= now;
  }
  if ((now - previous_5Secondmills > Interval_5Second) )
  {
        esp_task_wdt_reset();
        time_t startRead = millis();
        time_t endTime ;
        parameters = simpleCli.outputStream;
        endTime = millis();             // take 300ms
        simpleCli.outputStream->printf("\ntime:%ld  (%ldmili)\n",loopCount, endTime - startRead);
        vTaskDelay(10);
        //AD5940_Main(parameters); 
        loopCount++;
        previous_5Secondmills = millis();
  }
  if ((now - previous_30Secondmills > Interval_30Second))
  {
    previous_30Secondmills= now;
  }
  if ((now - previous_60Secondmills > Interval_60Second))
  {
    previous_60Secondmills= now;
  }
  vTaskDelay(100);
}