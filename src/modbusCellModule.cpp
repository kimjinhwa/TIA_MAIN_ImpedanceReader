#include "modbusCellModule.h"
#include "ModbusClientRTU.h"
#include "ModbusServerRTU.h"
#include "modbusRtu.h"
#include "AD5940.h"

#define CELLOFF 0x0000
#define CELLON 0xFF00
extern ModbusServerRTU LcdCell485;
extern ModbusServerRTU external485;
extern uint8_t selecectedCellNumber ;
void AD5940_ShutDown();

uint16_t sendGetModuleId(uint8_t modbusId, uint8_t fCode);
uint16_t sendGetChangeModuleId(uint8_t modbusId, uint8_t fCode);
void handleData(ModbusMessage response, uint32_t token);
void handleError(Error error, uint32_t token) ;

ModbusClientRTU modBusRtuCellModule(CELL485_DE,100);
static uint32_t request_response ;
static bool data_ready = false;
static char TAG[]="CELL MODULE";

modbus_cellRelay_t modbusCellrelay= {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

modbus_cellData_t modbusCellData= {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

modbus_data_t writeHoldRegister;
void modbusCellModuleSetup()
{
  modBusRtuCellModule.onDataHandler(&handleData);
  modBusRtuCellModule.onErrorHandler(&handleError);
  modBusRtuCellModule.setTimeout(1000);
  modBusRtuCellModule.begin(Serial2,BAUDRATESERIAL2,1);
}

void handleData(ModbusMessage response, uint32_t token)
{
    // First value is on pos 3, after server ID, function code and length byte
    uint16_t offs = 3;
    // The device has values all as IEEE754 float32 in two consecutive registers
    // Read the requested in a loop
    uint16_t *values;
    uint8_t len;
    uint8_t func = response.getFunctionCode();

    // ESP_LOGI("modbus", "Data Received: token %d Modbusid %d Func %d Error:%d",
    //   token, response.getServerID(),response.getFunctionCode(),response.getError());
    if (func == READ_COIL)
    {
      //uint8_t revData;
      modbusCellrelay.funcCode = func;
      modbusCellrelay.modbusId=response.getServerID();
      offs = 2;
      offs = response.get(offs, len);
      const uint8_t *d =response.data();
      modbusCellrelay.bitData = d[3];
      //ESP_LOGI("modbus","Cell Data %d",modbusCellrelay.bitData);
      request_response = token;
      data_ready = true;
    }
    if (func == READ_INPUT_REGISTER || func == READ_HOLD_REGISTER)
    {   
        offs = 2;
        offs = response.get(offs,len);
        
        // std::vector<uint8_t> MM_data(response.data(),response.data()+response.size());
        // for (uint8_t byte : MM_data) {
        //   ESP_LOGI(TAG,"%d",byte);
        // }
        //len = values[2] =static_cast<int>(MM_data[2])<<8|static_cast<int>(MM_data[3]);
        offs = 3;
        //ESP_LOGI(TAG, "func %d, dataLength %d ",func,len);
        values = (uint16_t *)&modbusCellData;
        for (uint8_t i = 0; i < len; i++)
        {
            offs = response.get(offs, values[i]);
        }
        request_response = token;
        data_ready = true;
    }
    else if (func == WRITE_COIL){
      //01 05 00 00 FF 00 8C 3A 
        int16_t address;
        int16_t data;
        values = (uint16_t *)&modbusCellrelay;
        values[0]=response.getServerID();
        values[1]=response.getFunctionCode();
        offs = 2;
        offs = response.get(offs, address = values[2]); //address
        offs = response.get(offs, data ); //data
        if(address==0)values[3]=data;
        else values[4]=data;
        // ESP_LOGI(TAG, "WRITE_COIL id: %d func %d,add:%d d1:0x%4x d2:0x%04x",
        //   response.getServerID(), response.getFunctionCode(),values[2],values[3], values[4]);
        request_response = token;
        data_ready = true;
    }
    else if (func == WRITE_HOLD_REGISTER){
      //01 05 00 00 FF 00 8C 3A 
        int16_t address;
        int16_t data;
        values = (uint16_t *)&writeHoldRegister;
        values[0]=response.getServerID();
        values[1]=response.getFunctionCode();
        std::vector<uint8_t> MM_data(response.data(),response.data()+response.size());
        for (uint8_t byte : MM_data) {
          ESP_LOGI(TAG,"%d",byte);
        }
        ESP_LOGI(TAG,"MM_data[2]%d",static_cast<int>(MM_data[2])<<8|static_cast<int>(MM_data[3]));
        ESP_LOGI(TAG,"MM_data[4]%d",static_cast<int>(MM_data[4])<<8|static_cast<int>(MM_data[5]));
        address = values[2] =static_cast<int>(MM_data[2])<<8|static_cast<int>(MM_data[3]);
        data = values[2] =static_cast<int>(MM_data[4])<<8|static_cast<int>(MM_data[5]);
        // offs = response.get(offs, address = values[2]); //address
        // offs = response.get(offs, data ); //data
        values[3]=data;
        ESP_LOGI(TAG, "WRITE_HOLDREGISTER id: %d func %d,add:%d d1:0x%04x ",
          response.getServerID(), response.getFunctionCode(),values[2],values[3] );
        request_response = token;
        data_ready = true;
    }
}
void handleError(Error error, uint32_t token) 
{
  // ModbusError wraps the error code and provides a readable error message for it
  ModbusError me(error);
  ESP_LOGE("MODBUS","Error response: %02X - %s\n", (int)me, (const char *)me);
}
void ClearoutReceviveData(HardwareSerial *serial,int timeout_ms)
{
  int c;
  int _timeout = 0;
  unsigned long fTimeOut = millis();
  while (!Serial2.available())
  {
    if (millis() - fTimeOut > timeout_ms) return ;  
    delay(1);
  };
  while(serial->available())
  {
    while (serial->available())
    {
      c = serial->read();
      //ESP_LOGW(TAG, "-->%02x", c);
      _timeout += 5;
      vTaskDelay(5);
      if (_timeout > timeout_ms)
        break;
    }
    _timeout += 5;
    if (_timeout > timeout_ms)
      break;
    vTaskDelay(5);
  }
}
void clearSerialGarbageData(HardwareSerial *serial,int timeout_ms){
  int c;
  int _timeout = 0;
  if(Serial2.available())ESP_LOGI(TAG,"Garbage Data : ");
  while(serial->available())
  {
    while (serial->available())
    {
      c = serial->read();
      ESP_LOGI(TAG, " %02x", c);
      ESP_LOGI(TAG, ".");
      _timeout += 5;
      vTaskDelay(5);
      if (_timeout > timeout_ms)
        break;
    }
    if (serial->available())
    {
      c = serial->read();
      ESP_LOGI(TAG, " %02x", c);
    }
    _timeout += 5;
    vTaskDelay(5);
    if (_timeout > timeout_ms)
      break;
  }
};
/* bool CellOnOff(uint8_t modbusId, uint16_t relay, uint16_t onoff) */
/* modbusID : 255를 주면 모든설치된 모듈에 대해서 실행한다. 
   relay : relay No. 0 or 1
   onoff : relay status
*/
bool CellOnOff(uint8_t modbusId, uint16_t relay, uint16_t onoff)
{
  int i;
  uint16_t retValue;
  uint16_t startId, endId;
  if (modbusId == 255)
  {
    startId = 1;
    endId = systemDefaultValue.installed_cells+1; //설치된 셀보다 하나의 모듈이 더 있다.
  }
  else
  {
    startId = modbusId;
    endId = modbusId;
  }
  for (i = startId; i <= endId; i++)
  {
    retValue = sendGetModbusModuleData(millis(), i, WRITE_COIL, relay, onoff);
    if (retValue != 0 )
    {
      // ESP_LOGE("MODULE", "Succeed %d", retValue);
    }
    else
    {
      ESP_LOGE("MODULE", "Fail Cell ID %d, Error %d d1:%04x d2:%04x", 
        i, retValue, modbusCellrelay.relay1,modbusCellrelay.relay2);
      return false;
    }
  }
  return true;
}
int readModuleRelayStatus(uint8_t modbusId)
{
  uint16_t checkSum;
  uint16_t retValue = 0;
  uint32_t token;
  AD5940_ShutDown();        // 전류의 흐름을 없애기 위하여 혹시 파형을 출력 중이면 정지 시킨다.
  LcdCell485.suspendTask(); //
  extendSerial.selectCellModule(true);
  vTaskDelay(10);
  token = millis();
  data_ready = false;
  ModbusMessage response = modBusRtuCellModule.syncRequest(token,
                               modbusId, READ_COIL, 0, 4);
  if (response .getError() == 0) // 에러가 없으면.
     handleData(response , token);
  else {
     ESP_LOGE("MODULE", "MODBUS Error" );
    extendSerial.selectLcd();
    LcdCell485.resumeTask();
     return -1;
  }
  //데이타는 여기에 수신이 된다. 
  //modbusCellrelay.bitData
  extendSerial.selectLcd();
  LcdCell485.resumeTask();
  return modbusCellrelay.bitData;
};
/*isModuleAllOff() */
/* 정상적이면 OFF값인 12가 리턴된다 */
/* 설치되어 있는 모든셀에 대하여 검사한다. 
하나라도 통신이 안되거나 12값이 아니면 켜져 있는 것이다.*/
int isModuleAllOff(){
  int i;
  int moduleState;
  //12,12의 값이 리턴될것이다. 
  for (i = 1; i <= systemDefaultValue.installed_cells+1; i++)
  {
    moduleState = readModuleRelayStatus(i);
    //ESP_LOGW("MODULE", "Step3 %d moduleState %d",i,moduleState);
    if (moduleState == -1 || moduleState != 12)
    {
      // 통신에러이며 더이상 진행하지 않는다.
      //ESP_LOGI("MODBUS","ID %d CELL OFF OR COMM ERROR",i);
      return moduleState;
    }
  }
  return moduleState;
}
bool SelectBatteryMinusPlus(uint8_t modbusId)
{

  // First Step : All relay 0 -> off
  int i;
  uint16_t retValue;
  // P15V를 공급한다. 
  digitalWrite(RELAY_2, RELAY_OFF   );
  delay(100);
  digitalWrite(RELAY_1, RELAY_OFF   );
  delay(500);

  //for(i=1;i<=systemDefaultValue.installed_cells;i++)
  //ESP_LOGW("MODULE", "Step 1 All Relay 1 Off(%d) Command %d",i,systemDefaultValue.installed_cells);
  
  // 모든 모듈의 1번 릴레이를 OFF한다.
  //ESP_LOGW("MODULE", "Step1 All Relay 1 Off Command %d",systemDefaultValue.installed_cells);
  if(!CellOnOff(255,0,CELLOFF)) return false;
  // 모든 모듈의 2번 릴레이를 OFF한다.
  //ESP_LOGW("MODULE", "Step2 All Relay 1 Off Command %d",systemDefaultValue.installed_cells);
  if(!CellOnOff(255,1,CELLOFF))return false;
  vTaskDelay(200);

  int moduleState;
  if( isModuleAllOff() != 12){
    ESP_LOGW("MODULE", "Step3 Error %d moduleState %d",i,moduleState);
    return false; //12,12의 값이 리턴될것이다. 
  }
  // 주어진 modbusid의 1번 릴레이를 ON한다.
  if(!CellOnOff(modbusId,0,CELLON))return false;
  vTaskDelay(200);
  //9,12의 값이 리턴될것이다. 
  moduleState = readModuleRelayStatus(modbusId);
  if(moduleState!=9){
    ESP_LOGW("MODULE", "Step4 Error %d moduleState %d",i,moduleState);
    return false;// 9가 리턴되어야 한다.
  }
  moduleState = readModuleRelayStatus(modbusId+1);
  if(moduleState!=12)
  {
    ESP_LOGW("MODULE", "Step4 Error %d moduleState %d",i,moduleState);
    return false;// 12가 리턴되어야 한다.
  }

  // 주어진 modbusid의 다음번  2번 릴레이를 ON한다.
  if(!CellOnOff(modbusId+1,1,CELLON))return false;
  vTaskDelay(200);
  //9,6의 값이 리턴될것이다. 
  moduleState = readModuleRelayStatus(modbusId);
  if(moduleState!=9){
    ESP_LOGW("MODULE", "Step5 Error %d moduleState %d",i,moduleState);
    return false;// 9가 리턴되어야 한다.
  }
  moduleState = readModuleRelayStatus(modbusId+1);
  if(moduleState!=6){
    ESP_LOGW("MODULE", "Step5 Error %d moduleState %d",i,moduleState);
    return false;// 6가 리턴되어야 한다.
  }
  // 외부 배터리를 연결한다. 
  ESP_LOGI("MODULE", "Step4 ");
  digitalWrite(RELAY_1, RELAY_ON   );
  delay(1000);
  digitalWrite(RELAY_2, RELAY_ON   );
  delay(1000);

  return true;
}
uint16_t sendGetModuleId(uint8_t modbusId, uint8_t fCode)
{
  uint16_t retValue;
  retValue = sendGetModbusModuleData(millis(), 1, READ_INPUT_REGISTER, 0, 3);

  if (retValue != 0)
  {
    ESP_LOGE("MODULE", "Succeed %d modbusCellData %d", retValue,
             modbusCellData.modbusid);
    return modbusCellData.modbusid;
  }
  else
  {
    ESP_LOGE("MODULE", "Fail");
    return false;
  }
  return modbusCellData.modbusid;
};

uint16_t sendGetChangeModuleId(uint8_t modbusId, uint8_t fCode)
{//Focde is 06
  uint16_t checkSum ;
  uint16_t value ;
  data_ready = false;
  LcdCell485.suspendTask();
  vTaskDelay(100);
  uint8_t buf[64];
  data_ready = false;
  extendSerial.selectCellModule(true);
  uint16_t change_id=modbusId;
  vTaskDelay(10);
  // change_id=  sendGetModuleId(255,4);
  // ESP_LOGI("modbus","change module id %d ",modbusId);
  //makeWriteSingleRegister(buf,255,fCode, 1, modbusId);
  uint32_t token=millis();
  sendGetModbusModuleData(token,1,WRITE_HOLD_REGISTER,1,change_id );
  // extendSerial.selectLcd();
  // LcdCell485.resumeTask();
  //extendSerial.selectCellModule(true);
  //data_ready  = readResponseDataForBrodcast(modbusId,fCode, buf,8,500); 
  if (data_ready)
  {
    value = buf[4]*256  + buf[5] ;
    ESP_LOGI("modbus","modbus id was changed : %d", writeHoldRegister.data );
    return writeHoldRegister.data ;
  }
  else
  {
    value =0;
    ESP_LOGI("modbus","Receive Failed");
    return false;
  }
  // extendSerial.selectLcd();
  // LcdCell485.resumeTask();
  return value;

};
ModbusMessage  syncRequestCellModule(uint32_t token,uint8_t modbusId, uint8_t fCode,uint16_t startAddress, uint16_t len)
{
    uint16_t retValue=0;
    data_ready = false;
    LcdCell485.suspendTask(); //
    extendSerial.selectCellModule(true);
    vTaskDelay(1); // Select cell module and set can sendData;
    Error err;
    ModbusMessage rc  = modBusRtuCellModule.syncRequest(token, 
      modbusId, fCode, startAddress,  len);
    ESP_LOGI("modbus", "getTemp");
    if (err != SUCCESS)
    {
        ModbusError e(err);
        ESP_LOGE("MODBUS", "Error creating request: %02X - %s\n", (int)e, (const char *)e);
        extendSerial.selectLcd();
        LcdCell485.resumeTask();
        return rc   ;
    }
    handleData(rc,token);
    extendSerial.selectLcd();
    LcdCell485.resumeTask();
    return rc    ;
}
uint32_t sendGetModbusModuleData(uint32_t token,uint8_t modbusId, uint8_t fCode,uint16_t startAddress, uint16_t len)
{
    uint16_t checkSum;
    uint16_t retValue=0;

    data_ready = false;
    AD5940_ShutDown();  // 전류의 흐름을 없애기 위하여 혹시 파형을 출력 중이면 정지 시킨다.
    LcdCell485.suspendTask(); //
    extendSerial.selectCellModule(true);
    vTaskDelay(10);
    // Select cell module and set can sendData;
    // Dont care true or false, because modBusRtuCellModule will use probe pin
    if(modbusId != 255){
      ModbusMessage rc  = modBusRtuCellModule.syncRequest(token, modbusId, fCode, startAddress,  len);
      if(rc.getError() == 0) //에러가 없으면.
      //ModbusMessage rc = m.setMessage(std::forward<Args>(args) ...);
      handleData(rc,token);
    }
    else{
      //Error err  = modBusRtuCellModule.addRequest(token, modbusId, fCode, startAddress,  len);
      // for(int i =0;i<100;i++){
      //   vTaskDelay(10);
      //   if(data_ready)break;
      //   ESP_LOGI("modbus", "Waiting....%d",i);
      // }

    }
    if (data_ready)
    {
        //token이 같은지 검사하자
        // ESP_LOGI("modbus", " %d %d Modbusid %d Temperature %d baudrate %d",token, request_response ,
        //                     modbusCellData.modbusid,  modbusCellData.temperature, modbusCellData.baudrate);
        cellvalue[modbusCellData.modbusid- 1].temperature =modbusCellData.temperature ;
        retValue=request_response ;
        data_ready=false;
    }
    else
    {
        retValue=0;
    }
    if(modbusId == 255 )retValue=1;
    extendSerial.selectLcd();
    LcdCell485.resumeTask();
    return retValue ;
};

  // ModbusMessage rc  = modBusRtuCellModule.syncRequest(token, modbusId, WRITE_HOLD_REGISTER, 1,change_id );
  // handleData(rc,token);
//void ClearoutReceviveData(HardwareSerial *serial,int timeout_ms);
//void clearSerialGarbageData(HardwareSerial *serial,int timeout_ms);
//int readResponseData(uint8_t modbusId,uint8_t funcCode, uint8_t *buf,uint8_t len,uint16_t timeout);
//int makeRelayControllData(uint8_t *buf,uint8_t modbusId,uint8_t funcCode, uint16_t address, uint16_t len);
//uint16_t checkAlloff(uint32_t *failedBatteryNumberH,uint32_t *failedBatteryNumberL);
//bool sendSelectBatteryWithNoCheck(uint8_t modbusId);
//bool sendSelectBattery(uint8_t modbusId);
//int makeWriteSingleRegister(uint8_t *buf,uint8_t modbusId,uint8_t funcCode, uint16_t address, uint16_t data);
//int makeTemperatureData(uint8_t *buf,uint8_t modbusId,uint8_t funcCode, uint16_t address, uint16_t len);
// uint32_t sendGetModbusModuleData_old(uint8_t modbusId, uint8_t fCode)
// {
//   uint16_t checkSum ;
//   uint16_t value ;
//   bool data_ready = false;
//   //ESP_LOGI("main","request");
//   LcdCell485.suspendTask();
//   vTaskDelay(100);
//   uint8_t buf[64];
//   data_ready = false;
//   makeTemperatureData(buf,modbusId,fCode,0,2);
//   extendSerial.selectCellModule(false);
//   ESP_LOGI("modbus","getTemp");
//   data_ready  = readResponseData(modbusId,fCode, buf,9,500); 
//   if (data_ready)
//   {
//     value = buf[3]*256  + buf[4] ;
//     ESP_LOGI("modbus","%d:%02x %02x Temperature %d",modbusId-1,buf[3],buf[4], value);
//     cellvalue[modbusId - 1].temperature = value ;
//   }
//   else
//   {
//     value =0;
//     ESP_LOGI("modbus","Receive Failed");
//   }
//   extendSerial.selectLcd();
//   LcdCell485.resumeTask();
//   return value;
// };

  //bool addToQueue(uint32_t tok, uint16_t mod, uint16_t fun, uint16_t add, uint16_t len)

  // TaskHandle_t *lcd_h = LcdCell485.getTaskHandle() ;
  // // if(lcd_h != nullptr) ESP_LOGW("MODULE", "GetHandel succeed");
  // TaskHandle_t lcdHandle = *lcd_h;  
  // vTaskSuspend(lcdHandle );
  // vTaskDelay(10);
  // eTaskState state =eTaskGetState(lcdHandle );
  // ESP_LOGW("MODULE", "LcdCell485.isSuspendedTask() %d", state);

  //LcdCell485.isSuspendedTask();
  //ESP_LOGW("MODULE", "LcdCell485.isSuspendedTask() %d", 1);
  // for (i = 1; i <= systemDefaultValue.installed_cells+1; i++)
  // {
  //   moduleState = readModuleRelayStatus(i);
  //   ESP_LOGW("MODULE", "Step3 %d moduleState %d",i,moduleState);
  //   // if (moduleState == -1 || moduleState != 12)
  //   // {
  //   //   // 통신에러이며 더이상 진행하지 않는다.
  //   //   ESP_LOGI("MODBUS","ID %d CELL OFF OR COMM ERROR",i);
  //   //   return false;
  //   // }
  // }
  // 
// int readResponseDataForBrodcast(uint8_t modbusId,uint8_t funcCode, uint8_t *buf,uint8_t len,uint16_t timeout)
// {
//   //uint16_t timeout; //timeout = 300;
//   uint16_t readCount=0;
//   data_ready = false;
//   //우선 타임아웃정에 데이타가 도착하는지를 확인한다.
//   unsigned long fTimeOut = millis();
//   while (!Serial2.available())
//   {
//     delay(1);
//     if (millis() - fTimeOut > timeout)
//     {
//       vTaskDelay(1);
//       ESP_LOGI("REV", "It's timeout error");
//       return data_ready;
//     }
//   };
//   //문자간 interval 계산은 최소 3.5배가 되어야 한다. 
//   // interval = 35000000 / baudrate
//   // 3.5 * 10 bits * 1000us * 1000ms / baudrate
//   // so 7291 ,  
//   // uint32_t interval = 7300;
//   uint32_t interval = 10000; //좀 늘려 보자
//   unsigned long lastMicros = micros();
//   while (micros() - lastMicros < interval)  //이제 데이타는 있으므로 시작한다.
//   {
//     if (Serial2.available())
//     {
//       buf[readCount++] = Serial2.read();
//       lastMicros = micros();
//       // ESP_LOGI("REV","%02x ",buf[readCount -1]);
//     };
//     // delay(500.0/BAUDRATESERIAL2 );  //4800일때 약 1ms이된다.
//     if (readCount == len)
//     {
//       // data를 받았다. 이제 id, command ,checksum 체크섬이 같은지 보자.
//       // ESP_LOGI("REV","data received len is %02x ",len);
//       if (buf[0] == 255 && buf[1] == funcCode && RTUutils::validCRC(buf, len))
//       {
//         data_ready = true;
//         break;
//       }
//       else
//       {
//         // ESP_LOGI("REV"," modbusId %d funcCode %d  validCRC %x",buf[0] ,buf[1], RTUutils::validCRC(buf,len));
//         data_ready = false;
//         // vTaskDelay(3000);
//         break;
//       }
//     }
//     if (micros() - lastMicros > interval)
//     {
//       ESP_LOGI("RECEIVE","Time to retch lastMisros %ld  %ld",micros(),lastMicros);
//       data_ready = false;
//       break;
//     }
//   }
//   if (data_ready)
//   {
//     //ESP_LOGI("main","Data Good Recieved");
//   }
//   else
//   {
//     for (int i = 0; i < readCount; i++)
//     {
//       ESP_LOGW("main","%d:%02x ",i ,buf[readCount]);
//     }
//     ESP_LOGW("main", "Receive Failed %d received",readCount);
//   }
//   //리턴하기 전에 Garbage가 있으면 정리한다.
//   clearSerialGarbageData(&Serial2,300);
//   return data_ready;
// };
// int readResponseData(uint8_t modbusId,uint8_t funcCode, uint8_t *buf,uint8_t len,uint16_t timeout)
// {
//   //uint16_t timeout; //timeout = 300;
//   uint16_t readCount=0;
//   //우선 타임아웃정에 데이타가 도착하는지를 확인한다.
//   unsigned long fTimeOut = millis();
//   while (!Serial2.available())
//   {
//     if (millis() - fTimeOut > timeout)
//     {
//       vTaskDelay(1);
//       ESP_LOGI("REV", "It's timeout error");
//       //simpleCli.outputStream->printf("\r\nIt's ble timeout error");
//       return data_ready;  // false
//     }
//     delay(1);
//   };
//   //문자간 interval 계산은 최소 3.5배가 되어야 한다. 
//   // interval = 35000000 / baudrate
//   // 3.5 * 10 bits * 1000us * 1000ms / baudrate
//   // so 7291 ,  
//   // uint32_t interval = 7300;
//   uint32_t interval = 10000; //좀 늘려 보자
//   unsigned long lastMicros = micros();
//   while (micros() - lastMicros < interval)  //이제 데이타는 있으므로 시작한다.
//   {
//     if (Serial2.available())
//     {
//       buf[readCount++] = Serial2.read();
//       lastMicros = micros(); // ESP_LOGI("REV","%02x ",buf[readCount -1]);
//     }; // delay(500.0/BAUDRATESERIAL2 );  //4800일때 약 1ms이된다.
//     if (readCount == len)
//     { // data를 받았다. 이제 id, command ,checksum 체크섬이 같은지 보자.
//       // ESP_LOGI("REV","data received len is %02x ",len);
//       if (buf[0] == modbusId && buf[1] == funcCode && RTUutils::validCRC(buf, len)) { data_ready = true; break; }
//       else {
//         // ESP_LOGI("REV"," modbusId %d funcCode %d  validCRC %x",buf[0] ,buf[1], RTUutils::validCRC(buf,len)); data_ready = false;
//         // vTaskDelay(3000);
//         break;
//       }
//     }
//     if (micros() - lastMicros > interval)
//     {
//       ESP_LOGI("RECEIVE","Time to reach lastMisros %ld  %ld",micros(),lastMicros);
//       data_ready = false;
//       break;
//     }
//     vTaskDelay(1);
//   }
//   if (data_ready)
//   {
//     //ESP_LOGI("main","Data Good Recieved");
//   }
//   else
//   {
//     for (int i = 0; i < readCount; i++)
//     {
//       Serial.printf("%d:%02x ",i ,buf[readCount]);
//     }
//     ESP_LOGW("main", "Receive Failed %d received",readCount);
//   }
//   //리턴하기 전에 Garbage가 있으면 정리한다.
//   clearSerialGarbageData(&Serial2,300);
//   return data_ready;
// };
// int makeRelayControllData(uint8_t *buf,uint8_t modbusId,uint8_t funcCode, uint16_t address, uint16_t len)
// {
//   uint16_t checkSum ;
//   buf[0] =modbusId; buf[1] = funcCode;
//   buf[2] = (uint8_t)((address & 0xff00) >>8 ); 
//   buf[3] = (uint8_t)(address & 0x00ff); // Address는 0부터 
//   buf[4] = (uint8_t)((len & 0xff00) >>8) ; 
//   buf[5] = (uint8_t)(len & 0x00ff);  //  갯수는 2개
//   checkSum =  RTUutils::calcCRC(buf,6);
//   buf[6] = checkSum & 0x00FF;
//   buf[7] = checkSum >> 8    ;

//   extendSerial.selectCellModule(false);//change to read Mode
//   clearSerialGarbageData(&Serial2,300);
//   extendSerial.selectCellModule(true); vTaskDelay(1);
//   Serial2.write(buf,8);
//   Serial2.flush();
//   extendSerial.selectCellModule(false);  //change to read Mode
//   return 1;
// }
// uint16_t checkAlloff(uint32_t *failedBatteryNumberH,uint32_t *failedBatteryNumberL)
// {
//   uint16_t  totalRelayStatusValue;
//   uint16_t isOK ;
//   uint32_t temp32H,temp32L;
//   *failedBatteryNumberH= 0;
//   *failedBatteryNumberL= 0;
//   uint8_t buf[64];
//   uint16_t retryCount=5;

//   ESP_LOGI("modbus","checkAllOff");
//   for (int modbusId = 1; modbusId <= systemDefaultValue.installed_cells; modbusId++)
//   {
//     retryCount=5;
//     while(retryCount--){
//       vTaskDelay(100);
//       makeRelayControllData(buf, modbusId, READ_COIL, 0, 2);     // Read coil data 2 개
//       extendSerial.selectCellModule(false);                      // 읽기 모드로 전환
//       isOK = readResponseData(modbusId, READ_COIL, buf, 6, 500); // * 100 즉 0.3초 us buf[3]이 Relay 데이타 이다.
//       if(isOK)break;
//     }
//     if (isOK == 1)
//     {
//       if (modbusId < 32)
//       {
//         temp32L = 1;
//         temp32L = temp32L << (modbusId - 1);
//         *failedBatteryNumberL |= temp32L;
//       }
//       else
//       {
//         temp32H = 1;
//         temp32H = temp32H << (modbusId - 1);
//         *failedBatteryNumberH |= temp32H;
//       }
//     }
//     else
//     {
//       if (modbusId < 32)
//       {
//         temp32L  = 0;
//         temp32L = ~(1 << (modbusId - 1));
//         *failedBatteryNumberL &= temp32L;
//       }
//       else
//       {
//         temp32H = 0;
//         temp32H = ~(1 << (modbusId - 1));
//         *failedBatteryNumberH &= temp32H;
//       }
//       ESP_LOGI("OFF RELAY","%d battery not response..", modbusId);
//     }
//     totalRelayStatusValue += buf[3];
//   }
//   return totalRelayStatusValue;
// }
// bool sendSelectBatteryWithNoCheck(uint8_t modbusId)
// {
//   // Coil 명령를 사용하며
//   // 1. 0xFF 명령으로 전체 OUT명령을 준다.
//   // 2. 현재 설치되어 있는 모든셀들이 통신 가능하여야 하고 릴레이 설정값을 0 을 갖고 있어야 한다.  
//   // 3. modbusID를 켠다
//   // 4. modbusID+1를 켠다
//   // 에러가 없다면 정상적으로 켜졌을 것이고, 확인 루틴은 다음번에 셀을 선택할 때 한다 
//   AD5940_ShutDown();  // 전류의 흐름을 없애기 위하여 혹시 파형을 출력 중이면 정지 시킨다.
//   selecectedCellNumber = modbusId;
//   uint16_t checkSum;
//   LcdCell485.suspendTask();
//   uint8_t buf[64];
//   uint16_t readCount ;
//   uint32_t failedBatteryH, failedBatteryL;
//   uint16_t retValue;

//   makeRelayControllData(buf, 0xFF, WRITE_COIL, 0, 0x00); // 0xff BROADCAST
//   extendSerial.selectCellModule(false);                                     // 읽기 모드로 전환
//   ClearoutReceviveData(&Serial2,300);
//   //readCount = readResponseData(modbusId, READ_COIL, buf, 6, 500); // buf[3]이 Relay 데이타 이다.

//   vTaskDelay(100);
  
//   makeRelayControllData(buf, 0xFF, WRITE_COIL, 1, 0x00); // 0xff BROADCAST
//   extendSerial.selectCellModule(false);                                     // 읽기 모드로 전환
//   ClearoutReceviveData(&Serial2,300);
//   //readCount = readResponseData(modbusId, READ_COIL, buf, 6, 500); // buf[3]이 Relay 데이타 이다.
//   vTaskDelay(500);
//   //makeRelayControllData(buf, modbusId, READ_COIL, 0, 2); // Read coil data 2 개


//   //extendSerial.selectCellModule(false);                                     // 읽기 모드로 전환
//   //vTaskDelay(100);  //이값을 주고 나서야 릴레이가 제대로 동작하였다.
//   // retValue는 반드시 0이어야 하고...
//   // 총셀은 20셀으므로 상위 바이트는 0X000F
//   //  하위 바이트는 0XFFFF이어야 한다.
//   {
//     makeRelayControllData(buf, modbusId, WRITE_COIL, 0, 0xFF00); // 해당 셀을 ON 시킨다
//     extendSerial.selectCellModule(false);                                     // 읽기 모드로 전환
//     readCount = readResponseData(modbusId, WRITE_COIL, buf, 8, 500); // buf[3]이 Relay 데이타 이다.
//     if(readCount == 1){
//       ESP_LOGI(TAG,"relay %d Minus(-) ON ",modbusId);
//     }
//     // Verify
//     makeRelayControllData(buf, modbusId, READ_COIL, 0, 2);          // Read coil data 2 개
//     extendSerial.selectCellModule(false);                           // 읽기 모드로 전환
//     readCount = readResponseData(modbusId, READ_COIL, buf, 6, 500); // buf[3]이 Relay 데이타 이다.
//     ESP_LOGI(TAG, "relay %d Minus(-) ON Value is %d", modbusId, buf[3]);
//     if(readCount == 1 && buf[3]==1) readCount = 1;
//     else readCount = 0;
//     // Veryfy end
//     vTaskDelay(100);
//     makeRelayControllData(buf, modbusId + 1, WRITE_COIL, 1, 0xFF00); // 해당 셀을 ON 시킨다
//     readCount = readResponseData(modbusId+1, WRITE_COIL, buf, 8, 500); // buf[3]이 Relay 데이타 이다.
//     if(readCount == 1){
//       ESP_LOGI(TAG,"relay %d Plus(+) ON ",modbusId+1);
//     }
//     // Verify
//     makeRelayControllData(buf, modbusId+1, READ_COIL, 0, 2);          // Read coil data 2 개
//     extendSerial.selectCellModule(false);                           // 읽기 모드로 전환
//     readCount = readResponseData(modbusId+1, READ_COIL, buf, 6, 500); // buf[3]이 Relay 데이타 이다.
//     ESP_LOGI(TAG, "relay %d Plus(+) ON Value is %d", modbusId+1, buf[3]);
//     if(readCount == 1 && buf[3]==2) readCount = 1;
//     else readCount = 0;
//     vTaskDelay(200);
//   }
//   extendSerial.selectLcd();
//   LcdCell485.resumeTask();
//   return readCount ;
// }
// bool sendSelectBattery(uint8_t modbusId)
// {
//   // Coil 명령를 사용하며
//   // 1. 0xFF 명령으로 전체 OUT명령을 준다.
//   // 2. 현재 설치되어 있는 모든셀들이 통신 가능하여야 하고 릴레이 설정값을 0 을 갖고 있어야 한다.  
//   // 3. modbusID를 켠다
//   // 4. modbusID+1를 켠다
//   // 에러가 없다면 정상적으로 켜졌을 것이고, 확인 루틴은 다음번에 셀을 선택할 때 한다 
//   AD5940_ShutDown();  // 전류의 흐름을 없애기 위하여 혹시 파형을 출력 중이면 정지 시킨다.
//   selecectedCellNumber = modbusId;
//   uint16_t checkSum;
//   LcdCell485.suspendTask();
//   uint8_t buf[64];
//   uint16_t readCount;
//   uint32_t failedBatteryH, failedBatteryL;
//   uint16_t retValue;

//   makeRelayControllData(buf, 0xFF, WRITE_COIL, 0, 0x00); // 0xff BROADCAST
//   extendSerial.selectCellModule(false);                                     // 읽기 모드로 전환
//   ClearoutReceviveData(&Serial2,300);
//   //readCount = readResponseData(modbusId, READ_COIL, buf, 6, 500); // buf[3]이 Relay 데이타 이다.
//   vTaskDelay(100);
  
//   makeRelayControllData(buf, 0xFF, WRITE_COIL, 1, 0x00); // 0xff BROADCAST
//   extendSerial.selectCellModule(false);                                     // 읽기 모드로 전환
//   ClearoutReceviveData(&Serial2,300);
//   //readCount = readResponseData(modbusId, READ_COIL, buf, 6, 500); // buf[3]이 Relay 데이타 이다.
//   vTaskDelay(500);


//   retValue = checkAlloff(&failedBatteryH, &failedBatteryL);
//   extendSerial.selectCellModule(false);                                     // 읽기 모드로 전환
//   vTaskDelay(200);  //이값을 주고 나서야 릴레이가 제대로 동작하였다.
//   // retValue는 반드시 0이어야 하고...
//   // 총셀은 20셀으므로 상위 바이트는 0X000F
//   //  하위 바이트는 0XFFFF이어야 한다.
//   uint32_t checkH = 0;
//   uint32_t checkL = 0;
//   for (int i = 0; i < systemDefaultValue.installed_cells; i++)
//   {
//     if (i < 32)
//       checkL |= (1U << i);
//     else
//       checkH |= (1U << i);
//   }
//   ESP_LOGI(TAG, "Relay ON State :%d %04x %04x %04x %04x", retValue, checkH, checkL, failedBatteryH, failedBatteryL);
//   if (retValue == 0 && checkH == failedBatteryH && checkL == failedBatteryL)
//   {
//     makeRelayControllData(buf, modbusId, WRITE_COIL, 0, 0xFF00);     // 해당 셀을 ON 시킨다
//     extendSerial.selectCellModule(false);                            // 읽기 모드로 전환
//     readCount = readResponseData(modbusId, WRITE_COIL, buf, 8, 500); // buf[3]이 Relay 데이타 이다.
//     if (readCount == 1)
//     {
//       ESP_LOGI(TAG, "relay %d Minus(-) ON ", modbusId);
//     }
//     // Verify
//     makeRelayControllData(buf, modbusId, READ_COIL, 0, 2);          // Read coil data 2 개
//     extendSerial.selectCellModule(false);                           // 읽기 모드로 전환
//     readCount = readResponseData(modbusId, READ_COIL, buf, 6, 500); // buf[3]이 Relay 데이타 이다.
//     ESP_LOGI(TAG, "relay %d Minus(-) ON Value is %d", modbusId, buf[3]);
//     if(readCount == 1 && buf[3]==1) readCount = 1;
//     else readCount = 0;
//     // Veryfy end
//     vTaskDelay(100);
//     makeRelayControllData(buf, modbusId + 1, WRITE_COIL, 1, 0xFF00);     // 해당 셀을 ON 시킨다
//     readCount = readResponseData(modbusId + 1, WRITE_COIL, buf, 8, 500); // buf[3]이 Relay 데이타 이다.
//     if (readCount == 1)
//     {
//       ESP_LOGI(TAG, "relay %d Plus(+) ON ", modbusId + 1);
//     }
//     // Verify
//     makeRelayControllData(buf, modbusId+1, READ_COIL, 0, 2);          // Read coil data 2 개
//     extendSerial.selectCellModule(false);                           // 읽기 모드로 전환
//     readCount = readResponseData(modbusId+1, READ_COIL, buf, 6, 500); // buf[3]이 Relay 데이타 이다.
//     ESP_LOGI(TAG, "relay %d Plus(+) ON Value is %d", modbusId+1, buf[3]);
//     if(readCount == 1 && buf[3]==2) readCount = 1;
//     else readCount = 0;
//     vTaskDelay(200);
//   }
//   else
//   {
//     ESP_LOGI(TAG,"All relay off fail or communication fails ");
//     readCount = 0;
//   }
//   extendSerial.selectLcd();
//   LcdCell485.resumeTask();
//   return readCount ;
// }
  // Focde is 06
  //  uint16_t checkSum ;
  //  uint16_t value ;
  //  data_ready = false;
  //  //ESP_LOGI("main","request");
  //  LcdCell485.suspendTask();
  //  vTaskDelay(100);
  //  uint8_t buf[64];
  //  data_ready = false;
  //  makeTemperatureData(buf,modbusId,fCode,0,2);
  //  extendSerial.selectCellModule(false);
  //  ESP_LOGI("modbus","getTemp");
  //  data_ready  = readResponseData(modbusId,fCode, buf,9,500);
  //  if (data_ready)
  //  {
  //    value = buf[5]*256  + buf[6] ;
  //  }
  //  else
  //  {
  //    value =0;
  //  }
  //  extendSerial.selectLcd();
  //  LcdCell485.resumeTask();
// int makeWriteSingleRegister(uint8_t *buf,uint8_t modbusId,uint8_t funcCode, uint16_t address, uint16_t data)
// {
//   //Header	None
// // 1 Slave Address	11
// // 2 Function	06
// // 3 Register Address Hi	00
// // 4 Register Address Lo	01
// // 5 Write Data Hi	00
// // 6 Write Data Lo	03
// // 7 Error Check Lo	9A	
// // 8 Error Check Hi	9B	
// //  Total Bytes	8
//   uint16_t checkSum ;
//   buf[0] = modbusId; 
//   buf[1] = funcCode;
//   buf[2] = (uint8_t)((address & 0xff00) >>8 ); 
//   buf[3] = (uint8_t)(address & 0x00ff); // Address는 0부터 
//   buf[4] = (uint8_t)((data & 0xff00) >>8) ; 
//   buf[5] = (uint8_t)(data & 0x00ff);  //  갯수는 2개
//   checkSum =  RTUutils::calcCRC(buf,6);
//   buf[6] = checkSum & 0x00FF;
//   buf[7] = checkSum >> 8    ;

//   extendSerial.selectCellModule(false);
//   clearSerialGarbageData(&Serial2,300);
//   extendSerial.selectCellModule(true);
//   vTaskDelay(5);
//   Serial2.write(buf,8);
//   Serial2.flush();
//   extendSerial.selectCellModule(false);
//   return 1;
// }
// int makeTemperatureData(uint8_t *buf,uint8_t modbusId,uint8_t funcCode, uint16_t address, uint16_t len)
// {
//   uint16_t checkSum ;
//   buf[0] =modbusId; 
//   buf[1] = funcCode;
//   buf[2] = (uint8_t)((address & 0xff00) >>8 ); 
//   buf[3] = (uint8_t)(address & 0x00ff); // Address는 0부터 
//   buf[4] = (uint8_t)((len & 0xff00) >>8) ; 
//   buf[5] = (uint8_t)(len & 0x00ff);  //  갯수는 2개
//   checkSum =  RTUutils::calcCRC(buf,6);
//   buf[6] = checkSum & 0x00FF;
//   buf[7] = checkSum >> 8    ;

//   extendSerial.selectCellModule(false);
//   clearSerialGarbageData(&Serial2,300);
//   extendSerial.selectCellModule(true);
//   vTaskDelay(5);
//   Serial2.write(buf,8);
//   Serial2.flush();
//   extendSerial.selectCellModule(false);
//   return 1;
// }
