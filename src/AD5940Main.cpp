/*!
 *****************************************************************************
 @file:    AD5940Main.c
 @author:  Neo Xu
 @brief:   Used to control specific application and process data.
 -----------------------------------------------------------------------------
Copyright (c) 2017-2019 Analog Devices, Inc. All Rights Reserved.
This software is proprietary to Analog Devices, Inc. and its licensors.
By using this software you agree to the terms of the associated
Analog Devices Software License Agreement.
 
*****************************************************************************/
/** 
 * @addtogroup AD5940_System_Examples
 * @{
 *  @defgroup Battery_Example
 *  @{
  */
#include <Arduino.h>
#include "ad5940.h"
#include <stdio.h>
#include "string.h"
#include "math.h"
#include "BATImpedance.h"
#include "ad5940.h"

#define MAX_LOOP_COUNT 30
#define APPBUFF_SIZE 512
uint32_t AppBuff[APPBUFF_SIZE];
char TAG[] = "AD5940";

extern uint8_t selecectedCellNumber ;
extern _cell_value cellvalue[MAX_INSTALLED_CELLS];
/* It's your choice here how to do with the data. Here is just an example to print them to UART */
fImpCar_Type pImpResult[31];
void addResult(uint32_t *pData, uint32_t DataCount)
{
  fImpCar_Type Average;
  fImpCar_Type *pImp = (fImpCar_Type *)pData;
  if(DataCount==10) {
    Average.Real=pImp->Real;Average.Image=pImp->Image;
  }
  pImpResult[DataCount].Real = pImp->Real;
  pImpResult[DataCount].Image= pImp->Image;
  if(DataCount == 29){
    for(int16_t i=10; i < DataCount;i++){
      Average.Real += pImpResult[i].Real;
      Average.Real /= 2.0;
      Average.Image+= pImpResult[i].Image;
      Average.Image/= 2.0;
    }
    ESP_LOGI("AVERAGE","Average(real, image) = , %f ,%f ,%f mOhm \n", Average.Real,Average.Image,AD5940_ComplexMag(&Average));
    cellvalue[selecectedCellNumber-1].impendance = AD5940_ComplexMag(&Average) ;
  }
}
int32_t BATShowResult(uint32_t *pData, uint32_t DataCount)
{
  fImpCar_Type *pImp = (fImpCar_Type*)pData;
	float freq;
	AppBATCtrl(BATCTRL_GETFREQ, &freq);
  /*Process data*/
  for(int i=0;i<DataCount;i++)
  {
    printf("Freq: %f (real, image) = ,%f , %f ,%f mOhm \n",freq, pImp[i].Real,pImp[i].Image,AD5940_ComplexMag(&pImp[i]));
  }
  return 0;
}

/* Initialize AD5940 basic blocks like clock */
static int32_t AD5940PlatformCfg(void)
{
  CLKCfg_Type clk_cfg;
  FIFOCfg_Type fifo_cfg;
  AGPIOCfg_Type gpio_cfg;
  /* Use hardware reset */
  ESP_LOGI(TAG,"AD5940_HWReset()");
  AD5940_HWReset();
  /* Platform configuration */
  ESP_LOGI(TAG,"AD5940_Initialize()");
  AD5940_Initialize();
  /* Step1. Configure clock */
  clk_cfg.ADCClkDiv = ADCCLKDIV_1;
  clk_cfg.ADCCLkSrc = ADCCLKSRC_HFOSC;
  clk_cfg.SysClkDiv = SYSCLKDIV_1;
  clk_cfg.SysClkSrc = SYSCLKSRC_HFOSC; //on battery board, there is a 32MHz crystal.
  clk_cfg.HfOSC32MHzMode = bFALSE;
  clk_cfg.HFOSCEn = bTRUE;
  clk_cfg.HFXTALEn = bFALSE;
  clk_cfg.LFOSCEn = bTRUE;
  ESP_LOGI(TAG,"AD5940_CLKCfg()");
  AD5940_CLKCfg(&clk_cfg);
  /* Step2. Configure FIFO and Sequencer*/
  fifo_cfg.FIFOEn = bFALSE;
  fifo_cfg.FIFOMode = FIFOMODE_FIFO;
  fifo_cfg.FIFOSize = FIFOSIZE_4KB;                       /* 4kB for FIFO, The reset 2kB for sequencer */
  fifo_cfg.FIFOSrc = FIFOSRC_DFT;
  fifo_cfg.FIFOThresh = 4;//AppBATCfg.FifoThresh;        /* DFT result. One pair for RCAL, another for Rz. One DFT result have real part and imaginary part */
  ESP_LOGI(TAG,"AD5940_FIFOCfg()");
  AD5940_FIFOCfg(&fifo_cfg);                             /* Disable to reset FIFO. */
  fifo_cfg.FIFOEn = bTRUE;  
  ESP_LOGI(TAG,"AD5940_FIFOCfg()");
  AD5940_FIFOCfg(&fifo_cfg);                             /* Enable FIFO here */
  
  /* Step3. Interrupt controller */
  ESP_LOGI(TAG,"Step3. Interrupt controller ");
  AD5940_INTCCfg(AFEINTC_1, AFEINTSRC_ALLINT, bTRUE);           /* Enable all interrupt in Interrupt Controller 1, so we can check INTC flags */
  AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH, bTRUE);   /* Interrupt Controller 0 will control GP0 to generate interrupt to MCU */
  AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
  gpio_cfg.FuncSet = GP0_INT|GP2_SYNC;
  gpio_cfg.InputEnSet = AGPIO_Pin2;
  gpio_cfg.OutputEnSet = AGPIO_Pin0|AGPIO_Pin2 | AGPIO_Pin1;
  gpio_cfg.OutVal = 0;
  gpio_cfg.PullEnSet = 0;
  AD5940_AGPIOCfg(&gpio_cfg);
  AD5940_SleepKeyCtrlS(SLPKEY_UNLOCK);  /* Allow AFE to enter sleep mode. */
  //AD5940_SleepKeyCtrlS(SLPKEY_LOCK);  /* Allow AFE to enter sleep mode. */
  ESP_LOGI(TAG,"AD5940_SleepKeyCtrlS() ");
  delayMicroseconds(1000);
  return 0;
}

extern AppBATCfg_Type AppBATCfg ; 
void AD5940BATStructInit(void)
{
  AppBATCfg_Type *pBATCfg;
  AppBATGetCfg(&pBATCfg);
  pBATCfg->SeqStartAddr = 0;
  pBATCfg->MaxSeqLen = 512;
  pBATCfg->RcalVal = 56.0;  							/* Value of RCAL on EVAL-AD5941BATZ board is 50mOhm */
  pBATCfg->ACVoltPP = 300.0f;							/* Pk-pk amplitude is 300mV */
  pBATCfg->DCVolt = 1200.0f;							/* Offset voltage of 1.2V*/
  pBATCfg->DftNum = DFTNUM_8192;
  
  pBATCfg->FifoThresh = 2;      					/* 2 results in FIFO, real and imaginary part. */
	
	pBATCfg->SinFreq = 1000;									/* Sin wave frequency. THis value has no effect if sweep is enabled */
	
	pBATCfg->SweepCfg.SweepEn = bFALSE;			/* Set to bTRUE to enable sweep function */
	pBATCfg->SweepCfg.SweepStart = 900.0f;		/* Start sweep at 1Hz  */
	pBATCfg->SweepCfg.SweepStop = 1000.0f;	/* Finish sweep at 1000Hz */
	pBATCfg->SweepCfg.SweepPoints = 20;			/* 100 frequencies in the sweep */
	pBATCfg->SweepCfg.SweepLog = bTRUE;			/* Set to bTRUE to use LOG scale. Set bFALSE to use linear scale */
	
}
void AD5940_Main_init()
{
  uint16_t temp;
  uint16_t iCount = 0;
  fImpCar_Type beforRcalVolt;
  uint32_t startTime;
  ESP_LOGI(TAG, "AD5940_Main_init\n");
  AD5940PlatformCfg();
  AD5940BATStructInit();             /* Configure your parameters in this function */

  AD5940Err error = AppBATInit(AppBuff, APPBUFF_SIZE); /* Initialize BAT application. Provide a buffer, which is used to store sequencer commands */
  ESP_LOGI(TAG, "AppBATInit %d %s ",error ,error == AD5940ERR_OK ?"성공":"실패");


  // iCount = AD5940_WakeUp(50);
  // ESP_LOGI(TAG, "AD5940_Wakeup count is %d ",iCount);
  // vTaskDelay(50);
  // ESP_LOGI(TAG, "Chip Id : %d\n", AD5940_ReadReg(REG_AFECON_CHIPID));
}

/* Return RcalVolt magnitude 
* 
*/
float AD5940_calibration(float *real , float *image,Print *outputStream ){
  uint16_t loopCount=100 ;
  AD5940PlatformCfg();
  AD5940BATStructInit(); /* Configure your parameters in this function */
  AppBATInit(AppBuff, APPBUFF_SIZE);    /* Initialize BAT application. Provide a buffer, which is used to store sequencer commands */
  *real  = 0.0f; *image = 0.0f;
  while (loopCount--)
  {
    ESP_LOGI(TAG, "AppBATCtrl(BATCTRL_MRCAL, 0)\n");
    time_t startTime = millis();
    if (AD5940ERR_WAKEUP == AppBATCtrl(BATCTRL_MRCAL, 0))
    {
      ESP_LOGW(TAG, "\nWakeup Error..retry...");
    }; /* Measur RCAL each point in sweep */
    time_t endTime = millis();
    // ESP_LOGI("IMP", "RcalVolt Real Image IMP:%f\t %f\t %f (%dmills)",
    //          AppBATCfg.RcalVolt.Real,
    //          AppBATCfg.RcalVolt.Image,
    //          AD5940_ComplexMag(&AppBATCfg.RcalVolt),endTime-startTime);
    if(outputStream != nullptr)
    outputStream->printf("\r\n%d: R I Mag:%6.2f\t %6.2f\t %6.2f (%dmills)", 
             loopCount,
             AppBATCfg.RcalVolt.Real,
             AppBATCfg.RcalVolt.Image,
             AD5940_ComplexMag(&AppBATCfg.RcalVolt),endTime-startTime);
    delay(100);
    if(loopCount < 10){
      *real += AppBATCfg.RcalVolt.Real;
      *image += AppBATCfg.RcalVolt.Image;
    }
  };
  *real /= 10.0f;
  *image /=10.0f;
  return AD5940_ComplexMag(&AppBATCfg.RcalVolt);
}
void AD5940_Main(void *parameters)
{
  uint32_t temp;
  // AD5940PlatformCfg();
  // AD5940BATStructInit(); /* Configure your parameters in this function */
  // AppBATInit(AppBuff, APPBUFF_SIZE);    /* Initialize BAT application. Provide a buffer, which is used to store sequencer commands */
  //
  //ESP_LOGI(TAG, "Chip Id : %d\n", AD5940_ReadReg(REG_AFECON_CHIPID));
  // AppBATCfg.RcalVolt.Real = -107659;
  // AppBATCfg.RcalVolt.Image = 112141; 

  AppBATCfg.RcalVolt.Real = systemDefaultValue.real_Cal;
  AppBATCfg.RcalVolt.Image = systemDefaultValue.image_Cal; 

  // while (1)
  // {
  //   ESP_LOGI(TAG, "AppBATCtrl(BATCTRL_MRCAL, 0)\n");
  //   time_t startTime = millis();
  //   if (AD5940ERR_WAKEUP == AppBATCtrl(BATCTRL_MRCAL, 0))
  //   {
  //     ESP_LOGW(TAG, "\nWakeup Error..retry...");
  //   }; /* Measur RCAL each point in sweep */
  //   time_t endTime = millis();
  //   ESP_LOGI("IMP", "RcalVolt Real Image IMP:%f\t %f\t %f (%dmills)",
  //            AppBATCfg.RcalVolt.Real,
  //            AppBATCfg.RcalVolt.Image,
  //            AD5940_ComplexMag(&AppBATCfg.RcalVolt),endTime-startTime);
  //   delay(100);
  // };
  //
  //AppBATCtrl(BATCTRL_MRCAL, 0);     /* Measur RCAL each point in sweep */
  AD5940BATStructInit();             /* Configure your parameters in this function */

  AD5940Err error = AppBATInit(AppBuff, APPBUFF_SIZE); /* Initialize BAT application. Provide a buffer, which is used to store sequencer commands */
  ESP_LOGI(TAG, "AppBATInit %d %s ",error ,error == AD5940ERR_OK ?"성공":"실패");
  digitalWrite(AD636_SEL,HIGH);
  //30개를 읽고 
  //앞의 10개는 버리고 
  //뒤의 20개는 평균을 내서 
  uint16_t loopCount ;
  for(loopCount =0;loopCount < MAX_LOOP_COUNT ;loopCount++ )
  {
    /* Check if interrupt flag which will be set when interrupt occurred. */

    AppBATCtrl(BATCTRL_START, 0);
    while (AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH) == bFALSE)
      ;
    // if(AD5940_GetMCUIntFlag())
    {

      AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
      AD5940_ClrMCUIntFlag(); /* Clear this flag */
      temp = APPBUFF_SIZE;
      AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH, bTRUE);
      AppBATISR(AppBuff, &temp); /* Deal with it and provide a buffer to store data we got */
      AD5940_Delay10us(100000);
      addResult(AppBuff, loopCount);
      BATShowResult(AppBuff, temp); /* Print measurement results over UART */
      AD5940_SEQMmrTrig(SEQID_0);   /* Trigger next measurement ussing MMR write*/
    }
    AD5940_AGPIOToggle(AGPIO_Pin1);
  }
}

/**
 * @}
 * @}
 * */
