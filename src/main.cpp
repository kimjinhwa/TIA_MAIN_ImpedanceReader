#include <Arduino.h>
#include <SPI.h>
#include <EEPROM.h>
#include <wifi.h>

#include "filesystem.h"
#include "mainGrobal.h"

#include "stdio.h"
//#include "ADuCM3029.h"
#include "AD5940.h"
//#include "HardwareSerialExtention.h"
#include "HardwareSerial.h"
#include <BluetoothSerial.h>
#include "myBlueTooth.h"
#include "NetworkTask.h"
#include <RtcDS1302.h>

// 기본 vSPI와 일치한다
#define VSPI_MISO   MISO  // IO19
#define VSPI_MOSI   MOSI  // IO 23
#define VSPI_SCLK   SCK   // IO 18
#define VSPI_SS     SS    // IO 5

// //SPIClass * vspi = NULL;

//SPIClass SPI;
//uint32_t MCUPlatformInit(void *pCfg);
static const int spiClk = 1000000; // 1 MHz
static char TAG[] ="Main";
TaskHandle_t *h_pxNetworkTask;
TaskHandle_t *h_pxblueToothTask;
nvsSystemSet systemDefaultValue;
ThreeWire myWire(13, 14, 33); // IO, SCLK, CE
RtcDS1302<ThreeWire> Rtc(myWire);


void pinsetup()
{
    pinMode(READ_BATVOL, INPUT);
    // pinMode(SCK, OUTPUT);
    // pinMode(MISO, OUTPUT);
    // pinMode(MOSI, OUTPUT);

    pinMode(AD5940_ISR, OUTPUT);
    pinMode(SERIAL_SEL_ADDR0, OUTPUT);
    pinMode(SERIAL_SEL_ADDR1, OUTPUT);
    pinMode(SERIAL_TX2 , OUTPUT);
    pinMode(LED_OP, OUTPUT);

    pinMode(RTC1305_EN, OUTPUT);
    pinMode(AD636_SEL, OUTPUT);
    pinMode(CS_5940, OUTPUT);
    pinMode(EXT_485EN_1, OUTPUT);
    pinMode(RESET_N, OUTPUT);
    pinMode(RESET_5940, OUTPUT);
    pinMode(CELL485_DE, OUTPUT);


    digitalWrite(CS_5940, HIGH);
};

//HardwareSerial Serial1;
void AD5940_Main(void *parameters);
void AD5940_Main_init();

void wifiApmodeConfig()
{
}
void readnWriteEEProm()
{
  uint8_t ipaddr1;
  if (EEPROM.read(0) != 0x55)
  {

    systemDefaultValue.AlarmAmpere = 200;
    systemDefaultValue.alarmDiffCellVoltage = 200;
    systemDefaultValue.alarmHighCellVoltage = 14500;
    systemDefaultValue.alarmLowCellVoltage = 8500;
    systemDefaultValue.AlarmTemperature = 65;
    systemDefaultValue.cutoffHighCellVoltage = 14800;
    systemDefaultValue.cutoffLowCellVoltage = 6500;
    systemDefaultValue.GATEWAY =(uint32_t)IPAddress(192, 168, 0, 1); 
    systemDefaultValue.IPADDRESS =(uint32_t)IPAddress(192, 168, 0, 201); 
    systemDefaultValue.modbusId = 1;
    strncpy(systemDefaultValue.ssid ,"iptime_mbhong",20);
    strncpy(systemDefaultValue.ssid_password,"",10);
    systemDefaultValue.SUBNETMASK =(uint32_t)IPAddress(255, 255, 255, 0); 
    systemDefaultValue.TotalCellCount = 40;
    strncpy(systemDefaultValue.userid,"admin",10);
    strncpy(systemDefaultValue.userpassword,"admin",10);

    EEPROM.writeByte(0, 0x55);
    //EEPROM.writeBytes(1, (const byte *)&ipAddress_struct, sizeof(nvsSystemSet));
    EEPROM.commit();
  }
  EEPROM.readBytes(1, (byte *)&systemDefaultValue, sizeof(nvsSystemSet));
}
BluetoothSerial SerialBT;

void setRtcNewTime(RtcDateTime rtc){
  digitalWrite(CS_5940, HIGH);
  SPI.end();
  Rtc.Begin();
  delay(1);
  if (!Rtc.GetIsRunning())
  {
    printf("RTC was not actively running, starting now\r\n");
    Rtc.SetIsRunning(true);
  }
  else 
    printf("RTC was actively status running \r\n");
  // struct timeval tmv;
  // tmv.tv_usec = 0;
  // tmv.tv_sec = 0;
  //RtcDateTime now(tmv.tv_sec);
  Rtc.SetDateTime(rtc);
  myWire.end();
  SPI.begin(SCK,MISO,MOSI,CS_5940);
}
RtcDateTime getDs1302GetRtcTime(){
  digitalWrite(CS_5940, HIGH);
  SPI.end();
  Rtc.Begin();
  if (!Rtc.GetIsRunning())
  {
    printf("RTC was not actively running, starting now\r\n");
    Rtc.SetIsRunning(true);
  }
  else 
    printf("RTC was actively status running \r\n");
  delay(1);
  RtcDateTime nowTime = Rtc.GetDateTime();
  myWire.end();
  delay(1);
  SPI.begin(SCK,MISO,MOSI,CS_5940);
  return nowTime;
}
void setRtc()
{
  Rtc.Begin();
  RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
  printf("\r\ncompiled time is %d/%d/%d %d:%d:%d\r\n",compiled.Year(),compiled.Month(),compiled.Day(),compiled.Hour(),compiled.Minute(),compiled.Second());
  if (!Rtc.IsDateTimeValid())
  {
    printf("RTC lost confidence in the DateTime!\r\n");
    //Rtc.SetDateTime(compiled);
  }
  else 
    printf("RTC available in the DateTime!\r\n");
  if (Rtc.GetIsWriteProtected())
  {
    printf("RTC was write protected, enabling writing now\r\n");
    Rtc.SetIsWriteProtected(false);
  }
  else 
    printf("RTC enabling writing \r\n");
  if (!Rtc.GetIsRunning())
  {
    printf("RTC was not actively running, starting now\r\n");
    Rtc.SetIsRunning(true);
  }
  else 
    printf("RTC was actively status running \r\n");
  if (!Rtc.GetIsRunning())
  {
    printf("RTC was not actively running, starting now\r\n");
    Rtc.SetIsRunning(true);
  }
  else 
    printf("RTC was actively status running \r\n");

  RtcDateTime now = Rtc.GetDateTime();
  printf("\r\nnow time is %d/%d/%d %d:%d:%d\r\n",now.Year(),now.Month(),now.Day(),now.Hour(),now.Minute(),now.Second());
  if (now < compiled)
  {
    printf("\r\nSet data with compiled time");
    //Rtc.SetDateTime(compiled);
  }
  struct timeval tmv;
  tmv.tv_sec = now.TotalSeconds();
  tmv.tv_usec = 0;
  //time_t toUnixTime = now.Unix32Time();
  //시스템의 시간도 같이 맞추어 준다.
  settimeofday(&tmv, NULL);
  gettimeofday(&tmv, NULL);
  now = RtcDateTime(tmv.tv_sec);
  //now = tmv.tv_sec;
  printf("\r\nset and reread time is %d/%d/%d %d:%d:%d\r\n",now.Year(),now.Month(),now.Day(),now.Hour(),now.Minute(),now.Second());

  myWire.end();
}
#define SELECT_LCD do {digitalWrite(SERIAL_SEL_ADDR0,HIGH);digitalWrite(SERIAL_SEL_ADDR1,LOW); }while(0)
  
  
class ExtendSerial//: public HardwareSerial
{
  private:
    const uint8_t _addr0=23;
    const uint8_t _addr1=2;
    bool _addrValue0;
    bool _addrValue1;
  public:
    ExtendSerial(){// : HardwareSerial(uart_num) {
    //LCDSerial(uint8_t uart_num) : HardwareSerial(uart_num){
      pinMode(_addr0,OUTPUT);
      pinMode(_addr1,OUTPUT);
    };
    /* 송수신모드를 사용하기 위하여 사용산다. */
    /*  writeEnable true: 송신가능  */
    /* writeEnable false : 수신가능*/
    void selectCellModule(bool writeEnable){
      digitalWrite(_addr0,false);
      digitalWrite(_addr1,false); 
      digitalWrite(CELL485_DE, writeEnable);
    };
    void selectLcd(){
      digitalWrite(_addr0,true);
      digitalWrite(_addr1,false); 
    };
    void ReleaseLcd(){
      digitalWrite(_addr0,LOW);
      digitalWrite(_addr1,LOW); 
    };

};

void setup(){
  EEPROM.begin(100);
  readnWriteEEProm();
  pinsetup();
  Serial.begin(BAUDRATE);
  // 내부의 LCD와 셀의 온도및 릴레이를 위해 사용한다.
  Serial1.begin(BAUDRATE,SERIAL_8N1,SERIAL_RX1 ,SERIAL_TX1 );
  //외부 485통신에 사용한다.
  Serial2.begin(BAUDRATE,SERIAL_8N1,SERIAL_RX2 ,SERIAL_TX2 );
  ExtendSerial ExtendSerial;
  ExtendSerial.selectCellModule(true);

  SerialBT.begin("TIMP_Device_1");
  wifiApmodeConfig();
  lsFile.littleFsInitFast(0);
  setRtc();

  SPI.setFrequency(spiClk );
  SPI.begin(SCK,MISO,MOSI,CS_5940);
  pinMode(SS, OUTPUT); //VSPI SS
  AD5940_MCUResourceInit(0);
  AD5940_Main_init();
  delay(1000);
  ESP_LOGI(TAG, "System Started");

  //xTaskCreate(NetworkTask,"NetworkTask",5000,NULL,1,h_pxNetworkTask); //PCB 패턴문제로 사용하지 않는다.
  xTaskCreate(blueToothTask,"blueToothTask",5000,NULL,1,h_pxblueToothTask);
};

static unsigned long previousmills = 0;
static int everySecondInterval = 5000;
static unsigned long now;
void loop(void)
{
  now = millis(); if ((now - previousmills > everySecondInterval))
  {
    previousmills = now;
    ESP_LOGI(TAG, "Chip Id : %d\n", AD5940_ReadReg(REG_AFECON_CHIPID));
  }
}
    //Serial.println(i++);
    //Serial.outputStream->printf("\nAnalog Value %d",analogRead( READ_BATVOL));
    // Serial2.write(0x55);
    // if(Serial2.available()){
    //   Serial.outputStream->printf("\nSerial2 read %x",Serial2.read());
    // }
  // outputStream->printf("Hello AD5940-Build Time:%s\n",__TIME__);
  // log_i("");

    // digitalSet(HIGH);
    // delay(1000);
    // digitalSet(LOW);
  //void *param;
  //AD5940_Main(param);
  //spiCommand(SPI, 0b11001100);
/* Below functions are used to initialize MCU Platform */
/* 단순하게 Serial.begin(23400) 으로 사용한다.*/
uint32_t MCUPlatformInit(void *pCfg)
{
  //시리얼 통신을 설정한다.
  // int UrtCfg(int iBaud);

  // /*Stop watch dog timer(ADuCM3029)*/
  // pADI_WDT0->CTL = 0xC9;
  // /* Clock Configure */
  // pADI_CLKG0_OSC->KEY = 0xCB14;               // Select HFOSC as system clock.
  // pADI_CLKG0_OSC->CTL =                       // Int 32khz LFOSC selected in LFMUX
  //   BITM_CLKG_OSC_CTL_HFOSCEN|BITM_CLKG_OSC_CTL_HFXTALEN;

  // while((pADI_CLKG0_OSC->CTL&BITM_CLKG_OSC_CTL_HFXTALOK) == 0);

  // pADI_CLKG0_OSC->KEY = 0xCB14; 
  // pADI_CLKG0_CLK->CTL0 = 0x201;                   /* Select XTAL as system clock */
  // pADI_CLKG0_CLK->CTL1 = 0;                   // ACLK,PCLK,HCLK divided by 1
  // pADI_CLKG0_CLK->CTL5 = 0x00;                 // Enable clock to all peripherals - no clock gating

  // UrtCfg(230400);/*Baud rate: 230400*/
  return 1;
}

/**
	@brief int UrtCfg(int iBaud, int iBits, int iFormat)
			==========Configure the UART.
	@param iBaud :{B1200,B2200,B2400,B4800,B9600,B19200,B38400,B57600,B115200,B230400,B430800}	\n
		Set iBaud to the baudrate required:
		Values usually: 1200, 2200 (for HART), 2400, 4800, 9600,
		        19200, 38400, 57600, 115200, 230400, 430800, or type in baud-rate directly
	@note
		- Powers up UART if not powered up.
		- Standard baudrates are accurate to better than 0.1% plus clock error.\n
		- Non standard baudrates are accurate to better than 1% plus clock error.
   @warning - If an external clock is used for the system the ullRtClk must be modified with \n
         the speed of the clock used.
**/

int UrtCfg(int iBaud)
{
  // int iBits = 3;//8bits, 
  // int iFormat = 0;//, int iBits, int iFormat
  // int i1;
  // int iDiv;
  // int iRtC;
  // int iOSR;
  // int iPllMulValue;
  // unsigned long long ullRtClk = 16000000;                // The root clock speed


  // /*Setup P0[11:10] as UART pins*/
  // pADI_GPIO0->CFG = (1<<22)|(1<<20)|(pADI_GPIO0->CFG&(~((3<<22)|(3<<20))));

  // iDiv = (pADI_CLKG0_CLK->CTL1& BITM_CLKG_CLK_CTL1_PCLKDIVCNT);                 // Read UART clock as set by CLKCON1[10:8]
  // iDiv = iDiv>>8;
  // if (iDiv == 0)
  //   iDiv = 1;
  // iRtC = (pADI_CLKG0_CLK->CTL0& BITM_CLKG_CLK_CTL0_CLKMUX); // Check what is the root clock

  // switch (iRtC)
  // {
  // case 0:                                               // HFOSC selected
  //   ullRtClk = 26000000;
  //   break;

  // case 1:                                               // HFXTAL selected
  //   if ((pADI_CLKG0_CLK->CTL0 & 0x200)==0x200)           // 26Mhz XTAL used
  //       ullRtClk = 26000000;
  //   else
  //       ullRtClk = 16000000;                              // Assume 16MHz XTAL
  //   break;

  // case 2:                                               // SPLL output
  //   iPllMulValue = (pADI_CLKG0_CLK->CTL3 &             // Check muliplication factor in PLL settings
  //                   BITM_CLKG_CLK_CTL3_SPLLNSEL);      // bits[4:0]. Assume div value of 0xD in bits [14:11]
  //   ullRtClk = (iPllMulValue *1000000);                // Assume straight multiplication by pADI_CLKG0_CLK->CTL3[4:0]
  //   break;

  // case 3:
  //   ullRtClk = 26000000;                                //External clock is assumed to be 26MhZ, if different
  //   break;                                             //clock speed is used, this should be changed

  // default:
  //   break;
  // }
  // //   iOSR = (pADI_UART0->COMLCR2 & 0x3);
  // //   iOSR = 2^(2+iOSR);
  // pADI_UART0->COMLCR2 = 0x3;
  // iOSR = 32;
  // //i1 = (ullRtClk/(iOSR*iDiv))/iBaud;	              // UART baud rate clock source is PCLK divided by OSR
  // i1 = (ullRtClk/(iOSR*iDiv))/iBaud-1;   //for bigger M and N value
  // pADI_UART0->COMDIV = i1;

  // pADI_UART0->COMFBR = 0x8800|(((((2048/(iOSR*iDiv))*ullRtClk)/i1)/iBaud)-2048);
  // pADI_UART0->COMIEN = 0;
  // pADI_UART0->COMLCR = (iFormat&0x3c)|(iBits&3);


  // pADI_UART0->COMFCR = (BITM_UART_COMFCR_RFTRIG & 0/*RX_FIFO_1BYTE*/ ) |BITM_UART_COMFCR_FIFOEN;
  // pADI_UART0->COMFCR |= BITM_UART_COMFCR_RFCLR|BITM_UART_COMFCR_TFCLR;                                   // Clear the UART FIFOs
  // pADI_UART0->COMFCR &= ~(BITM_UART_COMFCR_RFCLR|BITM_UART_COMFCR_TFCLR);                                // Disable clearing mechanism

  // NVIC_EnableIRQ(UART_EVT_IRQn);              // Enable UART interrupt source in NVIC
  // pADI_UART0->COMIEN = BITM_UART_COMIEN_ERBFI|BITM_UART_COMIEN_ELSI; /* Rx Interrupt */
  //return pADI_UART0->COMLSR;
  return 1;
}
//#include "stdio.h"
// #ifdef __ICCARM__
// int putchar(int c)
// #else
// int fputc(int c, FILE *f)
// #endif
// {
//   pADI_UART0->COMTX = c;
//   while((pADI_UART0->COMLSR&0x20) == 0);// tx fifo empty
//   return c;
// }

// void digitalSet(bool bSet){
//     // digitalWrite(SCK, bSet);
//     // digitalWrite(MISO, bSet);
//     // digitalWrite(MOSI, bSet);
//     // digitalWrite(AD5940_ISR, bSet);
//     // digitalWrite(SERIAL_SEL_ADDR0, bSet);
//     // digitalWrite(SERIAL_SEL_ADDR1, bSet);
//     // digitalWrite(RTC1305_EN, bSet);
//     // digitalWrite(AD636_SEL, bSet);
//     // digitalWrite(CS_5940, bSet);
//     // digitalWrite(EXT_485EN_1, bSet);
//     // digitalWrite(RESET_N, bSet);
//     // digitalWrite(RESET_5940, bSet);
//     // digitalWrite(CELL485_DE, bSet);
//     // digitalWrite(LED_OP, bSet);
// }
  // while(1){
  //   Serial.println("LCD Test");
  //   Serial2.println("LCD Test");
  //   if(Serial2.available()){
  //     Serial2.printf("%c",Serial2.read());
  //   }
  //   delay(1000);
  // }
  // while(1){
  //   Serial.println("LCD Test");
  //   Serial2.println("LCD Test");
  //   if(Serial2.available()){
  //     Serial.printf("%c",Serial2.read());
  //   }
  //   delay(1000);
  // }