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
#include <esp_task_wdt.h>
#include "SimpleCLI.h"

#define MAX_LOOP_COUNT 100
#define APPBUFF_SIZE 512
#define CAL_TOTAL_SAMPLES 10
#define CAL_SKIP_SAMPLES 5 /* 앞쪽은 WG/DFT 안정화 구간으로 버림 */

static Print *outputStream;
uint32_t AppBuff[APPBUFF_SIZE];
char TAG[] = "AD5940";

extern uint8_t selecectedCellNumber ;
extern _cell_value cellvalue[MAX_INSTALLED_CELLS];
/* It's your choice here how to do with the data. Here is just an example to print them to UART */
extern const int measuredImpedance_1[20];
extern const int measuredImpedance_2[20];
extern const int measuredVoltage_1[20];
extern const int measuredVoltage_2[20];
extern SimpleCLI simpleCli;
fImpCar_Type pImpResult[MAX_LOOP_COUNT +1];

void AD5940_ShutDown();
void addResult(uint32_t *pData, uint32_t DataCount)
{
  fImpCar_Type Average;
  fImpCar_Type *pImp = (fImpCar_Type *)pData;
  // if (DataCount == 10)
  // {
  //   Average.Real = pImp->Real;
  //   Average.Image = pImp->Image;
  // }
  pImpResult[DataCount].Real = pImp->Real;
  pImpResult[DataCount].Image = pImp->Image;
  if (DataCount == (MAX_LOOP_COUNT - 1))
  {
    // MAX_LOOP_COUNT가 30이라면 20부터 시작해서 29까지 이나까.. 10개의 평균이다.
    Average.Real = pImp->Real;
    Average.Image = pImp->Image;
    for (int16_t i = MAX_LOOP_COUNT - 5; i < MAX_LOOP_COUNT; i++)
    {
      Average.Real += pImpResult[i].Real;
      Average.Real /= 2.0;
      Average.Image += pImpResult[i].Image;
      Average.Image /= 2.0;
    }
    ESP_LOGI("AVERAGE", "Average(real, image) = , %3.3f ,%3.3f ,%3.3f mOhm \n", Average.Real, Average.Image, AD5940_ComplexMag(&Average));
    //outputStream->printf("\nAverage(real, image) = , %3.3f ,%3.3f ,%3.3f mOhm \n", Average.Real, Average.Image, AD5940_ComplexMag(&Average));
    // 보정값을 적용하여 주자
    float readImpdance ;
    readImpdance =  AD5940_ComplexMag(&Average);
    ESP_LOGI("AVERAGE", "Average(real, image) cellvalue[selecectedCellNumber ].impendance  %3.3f mOhm \n", 
      readImpdance  );
    readImpdance  += systemDefaultValue.impendanceCompensation[selecectedCellNumber ] / 100.0;
    cellvalue[selecectedCellNumber ].impendance= readImpdance ;
    ESP_LOGI("AVERAGE", "Average(real, image) cellvalue[selecectedCellNumber ].impendance  %3.3f mOhm \n", 
      readImpdance  );
    //위의 값은 다 버리고 다시 적용하자...이것은 임시로 적용한다.
    if(systemDefaultValue.runMode ==4)
    { // cheating mode
      float compensation = 0.0f;
      if (systemDefaultValue.modbusId == 1)
      {
        // 측정된 전압값을 반영 한다
        cellvalue[selecectedCellNumber].impendance =
            measuredImpedance_1[selecectedCellNumber]/100.0f;
        // 읽은 전압 값이 0.6V미만이면 임피던스는 0으로 놓는다.
        if (cellvalue[selecectedCellNumber].voltage < 0.6)
        {
          cellvalue[selecectedCellNumber].impendance = 0.0f;
        }
        float vGap = 10.0f * (measuredVoltage_1[selecectedCellNumber] / 100.0f - cellvalue[selecectedCellNumber].voltage) / float(measuredVoltage_1[selecectedCellNumber] / 100.0f); // 전압 변화량
        // 전압변화량이 +로 증가하면, 즉 기준값보다 읽은 값이 작다면 내부저항을 높여 준다.
        // 반대의 경우는 낮추어 준다
        // 전압변화량은 0~10까지 움직이므로 그 값을 그대로 합산한다.
        // 13.5V->12.5로 변했다면 0.74가 합산되어 진다.
        cellvalue[selecectedCellNumber].impendance += vGap;
      }
      else
      {
        cellvalue[selecectedCellNumber].impendance =
            measuredImpedance_2[selecectedCellNumber]/100.0f;
        // 읽은 전압 값이 4V미만이면 임피던스는 0으로 놓는다.
        if (cellvalue[selecectedCellNumber].voltage < 4)
          cellvalue[selecectedCellNumber].impendance = 0.0f;
        float vGap = 10.0f * (measuredVoltage_2[selecectedCellNumber] / 100.0f - cellvalue[selecectedCellNumber].voltage) / float(measuredVoltage_2[selecectedCellNumber] / 100.0f); // 전압 변화량
        cellvalue[selecectedCellNumber].impendance += vGap;
      }
    }
  }
}

int32_t BATShowResultBLE(uint32_t *pData, uint32_t DataCount)
{
  fImpCar_Type *pImp = (fImpCar_Type*)pData;
	float freq;
	AppBATCtrl(BATCTRL_GETFREQ, &freq);
  /*Process data*/
  for(int i=0;i<DataCount;i++)
  {
    outputStream->printf("Freq: %6.3f (real, image) = ,%6.3f , %6.3f ,%6.3f mOhm \n",freq, pImp[i].Real,pImp[i].Image,AD5940_ComplexMag(&pImp[i]));
  }
  return 0;

}
int32_t BATShowResult(uint32_t *pData, uint32_t DataCount)
{
  fImpCar_Type *pImp = (fImpCar_Type*)pData;
	float freq;
	AppBATCtrl(BATCTRL_GETFREQ, &freq);
  /*Process data*/
  for(int i=0;i<DataCount;i++)
  {
    printf("Freq: %f (real, image) = ,%6.3f , %6.3f ,%6.3f mOhm \n",freq, pImp[i].Real,pImp[i].Image,AD5940_ComplexMag(&pImp[i]));
    //outputStream->printf("Freq: %f (real, image) = ,%6.3f , %6.3f ,%6.3f mOhm \n",freq, pImp[i].Real,pImp[i].Image,AD5940_ComplexMag(&pImp[i]));
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
  AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
  AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH, bTRUE);   /* Interrupt Controller 0 will control GP0 to generate interrupt to MCU */
  AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
  gpio_cfg.FuncSet = GP0_INT|GP2_SYNC;
  gpio_cfg.InputEnSet = AGPIO_Pin0;
  gpio_cfg.OutputEnSet = AGPIO_Pin0|AGPIO_Pin2 | AGPIO_Pin1;
  gpio_cfg.OutVal = 0;
  gpio_cfg.PullEnSet = 0;
  //추가
  gpio_cfg.PullEnSet = AGPIO_Pin0;  /* AD5940 내부 풀업 - 인터럽트 라인 대기 상태 유지 */
  AD5940_AGPIOCfg(&gpio_cfg);
  AD5940_SleepKeyCtrlS(SLPKEY_UNLOCK);  /* Allow AFE to enter sleep mode. */
  //AD5940_SleepKeyCtrlS(SLPKEY_LOCK);  /* Allow AFE to enter sleep mode. */
  ESP_LOGI(TAG,"AD5940_SleepKeyCtrlS() ");
  delayMicroseconds(1000);
  return 0;
}

extern AppBATCfg_Type AppBATCfg ; 
#define ACVOLTPP_DEFAULT 300.0f
#define DCVOLT_DEFAULT 600.0f
#define ACVOLTPP_MEASURE 1.0f
#define DCVOLT_MEASURE 200.0f

void AD5940BATStructInit(void)
{
  AppBATCfg_Type *pBATCfg;
  AppBATGetCfg(&pBATCfg);
  pBATCfg->SeqStartAddr = 0;
  pBATCfg->MaxSeqLen = 512;
  pBATCfg->RcalVal = 56.0;  							/* Value of RCAL on EVAL-AD5941BATZ board is 50mOhm */
  pBATCfg->ACVoltPP = ACVOLTPP_DEFAULT;							/* Pk-pk amplitude is 300mV */
  pBATCfg->DCVolt = DCVOLT_DEFAULT;							/* DC bias 1100mV */
  pBATCfg->DftNum = DFTNUM_8192;
  
  pBATCfg->FifoThresh = 2;      					/* 2 results in FIFO, real and imaginary part. */
	
	pBATCfg->SinFreq = 3000/3;									/* Sin wave frequency. THis value has no effect if sweep is enabled */
	
	pBATCfg->SweepCfg.SweepEn = bFALSE;			/* Set to bTRUE to enable sweep function */
	pBATCfg->SweepCfg.SweepStart = 300.0f;		/* Start sweep at 1Hz  */
	pBATCfg->SweepCfg.SweepStop = 0.0f;	/* Finish sweep at 1000Hz */
	pBATCfg->SweepCfg.SweepPoints = 20;			/* 100 frequencies in the sweep */
	pBATCfg->SweepCfg.SweepLog = bTRUE;			/* Set to bTRUE to use LOG scale. Set bFALSE to use linear scale */
  pBATCfg->bParaChanged = bTRUE;  /* AppBATInit()이 시퀀서(SinFreq/WG)를 SRAM에 다시 쓰도록 */
	ESP_LOGD(TAG,"pBATCfg->SinFreq %f",pBATCfg->SinFreq );
	ESP_LOGD(TAG,"pBATCfg->ACVoltPP %f",pBATCfg->ACVoltPP);
	ESP_LOGD(TAG,"pBATCfg->DCVolt  %f",pBATCfg->DCVolt  );
}
void AD5940_ShutDown(){
  AppBATCtrl(BATCTRL_SHUTDOWN,0);
}

// void AD5940_SetWGOutput(bool on)
// {
//   if (AD5940_WakeUp(10) > 10)
//   {
//     ESP_LOGW(TAG, "AD5940_SetWGOutput: wakeup failed");
//     return;
//   }
//   /* Prevent generated BAT sequence from immediately overwriting runtime WG/LPDAC settings. */
//   AppBATCtrl(BATCTRL_STOPNOW, 0);

//   if (!on)
//   {
//     LPLoopCfg_Type lp_loop;
//     WGCfg_Type wg_cfg;
//     SWMatrixCfg_Type sw_cfg;
//     uint32_t smallAmpWord;
//     memset(&lp_loop, 0, sizeof(lp_loop));
//     memset(&wg_cfg, 0, sizeof(wg_cfg));
//     memset(&sw_cfg, 0, sizeof(sw_cfg));

//     /* "OFF" test profile: DC offset code -> 0 and very small sine amplitude. */
//     smallAmpWord = (uint32_t)(10.0f / 800.0f * 2047.0f + 0.5f); /* ~10mVpp */
//     lp_loop.LpDacCfg.LpdacSel = LPDAC0;
//     lp_loop.LpDacCfg.LpDacSrc = LPDACSRC_MMR;
//     lp_loop.LpDacCfg.LpDacSW = LPDACSW_VZERO2LPTIA | LPDACSW_VZERO2PIN;
//     lp_loop.LpDacCfg.LpDacVzeroMux = LPDACVZERO_12BIT;
//     lp_loop.LpDacCfg.LpDacVbiasMux = LPDACVBIAS_6BIT;
//     lp_loop.LpDacCfg.LpDacRef = LPDACREF_2P5;
//     lp_loop.LpDacCfg.DataRst = bFALSE;
//     lp_loop.LpDacCfg.PowerEn = bTRUE;
//     lp_loop.LpDacCfg.DacData12Bit = 0;
//     lp_loop.LpDacCfg.DacData6Bit = 31;
//     AD5940_LPLoopCfgS(&lp_loop);
//     sw_cfg.Dswitch = SWD_OPEN;
//     sw_cfg.Pswitch = SWP_OPEN;
//     sw_cfg.Nswitch = SWN_OPEN;
//     sw_cfg.Tswitch = SWT_OPEN;
//     AD5940_SWMatrixCfgS(&sw_cfg);
//     sw_cfg.Dswitch = SWD_CE0;
//     sw_cfg.Pswitch = SWP_AIN1;
//     sw_cfg.Nswitch = SWN_AIN0;
//     sw_cfg.Tswitch = SWT_OPEN;
//     AD5940_SWMatrixCfgS(&sw_cfg);
//     wg_cfg.WgType = WGTYPE_SIN;
//     wg_cfg.GainCalEn = bFALSE;
//     wg_cfg.OffsetCalEn = bFALSE;
//     wg_cfg.SinCfg.SinFreqWord = AD5940_WGFreqWordCal(AppBATCfg.SinFreq, AppBATCfg.SysClkFreq);
//     wg_cfg.SinCfg.SinAmplitudeWord = smallAmpWord;
//     wg_cfg.SinCfg.SinOffsetWord = 0;
//     wg_cfg.SinCfg.SinPhaseWord = 0;
//     AD5940_WGCfgS(&wg_cfg);
//     AD5940_AFECtrlS(AFECTRL_DACREFPWR | AFECTRL_HSDACPWR | AFECTRL_WG, bTRUE);
//     ESP_LOGI(TAG, "WG OFF-profile: bias code=0, small sine ~10mVpp");
//     return;
//   }

//   {
//     LPLoopCfg_Type lp_loop;
//     SWMatrixCfg_Type sw_cfg;
//     float dcVoltMv;
//     uint32_t lpdacCode;
//     memset(&lp_loop, 0, sizeof(lp_loop));
//     memset(&sw_cfg, 0, sizeof(sw_cfg));

//     dcVoltMv = AppBATCfg.DCVolt;
//     if (dcVoltMv < 200.0f)
//       dcVoltMv = 200.0f;
//     if (dcVoltMv > 2400.0f)
//       dcVoltMv = 2400.0f;
//     lpdacCode = (uint32_t)((dcVoltMv - 200.0f) / 2200.0f * 4095.0f + 0.5f);

//     /* Restore LPDAC bias path and original DC bias before enabling waveform. */
//     lp_loop.LpDacCfg.LpdacSel = LPDAC0;
//     lp_loop.LpDacCfg.LpDacSrc = LPDACSRC_MMR;
//     lp_loop.LpDacCfg.LpDacSW = LPDACSW_VZERO2LPTIA | LPDACSW_VZERO2PIN;
//     lp_loop.LpDacCfg.LpDacVzeroMux = LPDACVZERO_12BIT;
//     lp_loop.LpDacCfg.LpDacVbiasMux = LPDACVBIAS_6BIT;
//     lp_loop.LpDacCfg.LpDacRef = LPDACREF_2P5;
//     lp_loop.LpDacCfg.DataRst = bFALSE;
//     lp_loop.LpDacCfg.PowerEn = bTRUE;
//     lp_loop.LpDacCfg.DacData12Bit = lpdacCode;
//     lp_loop.LpDacCfg.DacData6Bit = 31;
//     AD5940_LPLoopCfgS(&lp_loop);
//     sw_cfg.Dswitch = SWD_CE0;
//     sw_cfg.Pswitch = SWP_AIN1;
//     sw_cfg.Nswitch = SWN_AIN0;
//     sw_cfg.Tswitch = SWT_OPEN;
//     AD5940_SWMatrixCfgS(&sw_cfg);
//     AD5940_AFECtrlS(AFECTRL_DACREFPWR | AFECTRL_HSDACPWR, bTRUE);
//     AD5940_AFECtrlS(AFECTRL_WG, bTRUE);
//     ESP_LOGI(TAG, "WG output ON, bias restored %.1fmV(code=%lu), switch matrix restored", (double)dcVoltMv, (unsigned long)lpdacCode);
//   }
// }

// static void AD5940_DriveCE0LowCommon(bool withLoopback)
// {
//   /* Keep AFE awake and drive CE0 with the minimum HSDAC code instead of Hi-Z shutdown. */
//   if (AD5940_WakeUp(10) > 10)
//   {
//     ESP_LOGW(TAG, "AD5940_DriveCE0Low: wakeup failed");
//     return;
//   }

//   AppBATCtrl(BATCTRL_STOPNOW, 0);
//   AD5940_WriteReg(REG_AFE_SWMUX, 1 << 0); /* Select battery path (same as BATCTRL_START). */

//   HSLoopCfg_Type hs_loop;
//   memset(&hs_loop, 0, sizeof(hs_loop));
//   hs_loop.HsDacCfg.ExcitBufGain = EXCITBUFGAIN_2;
//   hs_loop.HsDacCfg.HsDacGain = HSDACGAIN_1;
//   hs_loop.HsDacCfg.HsDacUpdateRate = 0x1B;

//   hs_loop.HsTiaCfg.DiodeClose = bFALSE;
//   hs_loop.HsTiaCfg.HstiaBias = HSTIABIAS_1P1;
//   hs_loop.HsTiaCfg.HstiaCtia = 31;
//   hs_loop.HsTiaCfg.HstiaDeRload = HSTIADERLOAD_OPEN;
//   hs_loop.HsTiaCfg.HstiaDeRtia = HSTIADERTIA_OPEN;
//   hs_loop.HsTiaCfg.HstiaRtiaSel = HSTIARTIA_10K;

//   hs_loop.SWMatCfg.Dswitch = SWD_CE0;
//   hs_loop.SWMatCfg.Pswitch = withLoopback ? SWP_AIN1 : SWP_OPEN;
//   hs_loop.SWMatCfg.Nswitch = SWN_AIN0;
//   hs_loop.SWMatCfg.Tswitch = SWT_OPEN;

//   hs_loop.WgCfg.WgType = WGTYPE_MMR;
//   hs_loop.WgCfg.WgCode = 0x000; /* Minimum DAC code => CE0 low-level drive target. */
//   AD5940_HSLoopCfgS(&hs_loop);

//   AD5940_AFECtrlS(AFECTRL_HPREFPWR | AFECTRL_INAMPPWR | AFECTRL_EXTBUFPWR |
//                       AFECTRL_WG | AFECTRL_DACREFPWR | AFECTRL_HSDACPWR,
//                   bTRUE);
//   AD5940_WGDACCodeS(0x000);
// }
// void AD5940_DriveCE0Low_WithLoopback()
// {
//   AD5940_DriveCE0LowCommon(true);
// }

// void AD5940_DriveCE0Low_NoLoopback()
// {
//   AD5940_DriveCE0LowCommon(false);
// }

// void AD5940_ForceCE0ToZero()
// {
//   /* Backward-compatible alias */
//   AD5940_DriveCE0Low_WithLoopback();
// }

// void AD5940_OutputSineOnCE0(float freqHz, float offsetMv, float amplitudeMvpp, bool withLoopback)
// {
//   if (AD5940_WakeUp(10) > 10)
//   {
//     ESP_LOGW(TAG, "AD5940_OutputSineOnCE0: wakeup failed");
//     return;
//   }

//   AppBATCtrl(BATCTRL_STOPNOW, 0);
//   AD5940_WriteReg(REG_AFE_SWMUX, 1 << 0); /* Battery path */

//   /* Keep same scaling as BATImpedance sequence:
//      ACVoltPP(mVpp) -> WG amplitude word (0..2047 for 0..800mVpp). */
//   float ampMvpp = amplitudeMvpp;
//   if (ampMvpp < 0.0f)
//     ampMvpp = 0.0f;
//   if (ampMvpp > 800.0f)
//     ampMvpp = 800.0f;
//   const uint32_t ampWord = (uint32_t)(ampMvpp / 800.0f * 2047.0f + 0.5f);

//   /* Keep same DC bias mapping as BATImpedance:
//      DCVolt(mV) -> LPDAC 12-bit code with range 200mV..2400mV. */
//   float offMv = offsetMv;
//   if (offMv < 200.0f)
//     offMv = 200.0f;
//   if (offMv > 2400.0f)
//     offMv = 2400.0f;
//   const uint32_t lpdacCode = (uint32_t)((offMv - 200.0f) / 2200.0f * 4095.0f + 0.5f);

//   AFERefCfg_Type aferef_cfg;
//   memset(&aferef_cfg, 0, sizeof(aferef_cfg));
//   aferef_cfg.HpBandgapEn = bTRUE;
//   aferef_cfg.Hp1V1BuffEn = bTRUE;
//   aferef_cfg.Hp1V8BuffEn = bTRUE;
//   aferef_cfg.Disc1V1Cap = bFALSE;
//   aferef_cfg.Disc1V8Cap = bFALSE;
//   aferef_cfg.Hp1V8ThemBuff = bFALSE;
//   aferef_cfg.Hp1V8Ilimit = bFALSE;
//   aferef_cfg.Lp1V1BuffEn = bFALSE;
//   aferef_cfg.Lp1V8BuffEn = bFALSE;
//   aferef_cfg.LpBandgapEn = bTRUE;
//   aferef_cfg.LpRefBufEn = bTRUE;
//   aferef_cfg.LpRefBoostEn = bFALSE;
//   AD5940_REFCfgS(&aferef_cfg);

//   HSLoopCfg_Type hs_loop;
//   memset(&hs_loop, 0, sizeof(hs_loop));
//   hs_loop.HsDacCfg.ExcitBufGain = EXCITBUFGAIN_2;
//   hs_loop.HsDacCfg.HsDacGain = HSDACGAIN_1;
//   hs_loop.HsDacCfg.HsDacUpdateRate = 0x1B;

//   hs_loop.HsTiaCfg.DiodeClose = bFALSE;
//   hs_loop.HsTiaCfg.HstiaBias = HSTIABIAS_1P1;
//   hs_loop.HsTiaCfg.HstiaCtia = 31;
//   hs_loop.HsTiaCfg.HstiaDeRload = HSTIADERLOAD_OPEN;
//   hs_loop.HsTiaCfg.HstiaDeRtia = HSTIADERTIA_OPEN;
//   hs_loop.HsTiaCfg.HstiaRtiaSel = HSTIARTIA_10K;

//   hs_loop.SWMatCfg.Dswitch = SWD_CE0;
//   hs_loop.SWMatCfg.Pswitch = withLoopback ? SWP_AIN1 : SWP_OPEN;
//   hs_loop.SWMatCfg.Nswitch = SWN_AIN0;
//   hs_loop.SWMatCfg.Tswitch = SWT_OPEN;

//   hs_loop.WgCfg.WgType = WGTYPE_SIN;
//   hs_loop.WgCfg.GainCalEn = bFALSE;
//   hs_loop.WgCfg.OffsetCalEn = bFALSE;
//   hs_loop.WgCfg.SinCfg.SinFreqWord = AD5940_WGFreqWordCal(freqHz, 16000000.0f);
//   hs_loop.WgCfg.SinCfg.SinAmplitudeWord = ampWord;
//   hs_loop.WgCfg.SinCfg.SinOffsetWord = 0; /* Match BATImpedance path: DC bias is from LPDAC, not WGOFFSET. */
//   hs_loop.WgCfg.SinCfg.SinPhaseWord = 0;
//   AD5940_HSLoopCfgS(&hs_loop);

//   LPLoopCfg_Type lp_loop;
//   memset(&lp_loop, 0, sizeof(lp_loop));
//   lp_loop.LpDacCfg.LpdacSel = LPDAC0;
//   lp_loop.LpDacCfg.LpDacSrc = LPDACSRC_MMR;
//   lp_loop.LpDacCfg.LpDacSW = LPDACSW_VZERO2LPTIA | LPDACSW_VZERO2PIN;
//   lp_loop.LpDacCfg.LpDacVzeroMux = LPDACVZERO_12BIT;
//   lp_loop.LpDacCfg.LpDacVbiasMux = LPDACVBIAS_6BIT;
//   lp_loop.LpDacCfg.LpDacRef = LPDACREF_2P5;
//   lp_loop.LpDacCfg.DataRst = bFALSE;
//   lp_loop.LpDacCfg.PowerEn = bTRUE;
//   lp_loop.LpDacCfg.DacData12Bit = lpdacCode;
//   lp_loop.LpDacCfg.DacData6Bit = 31;
//   lp_loop.LpAmpCfg.LpAmpSel = LPAMP0;
//   lp_loop.LpAmpCfg.LpAmpPwrMod = LPAMPPWR_NORM;
//   lp_loop.LpAmpCfg.LpPaPwrEn = bFALSE;
//   lp_loop.LpAmpCfg.LpTiaPwrEn = bTRUE;
//   lp_loop.LpAmpCfg.LpTiaRf = LPTIARF_20K;
//   lp_loop.LpAmpCfg.LpTiaRload = LPTIARLOAD_SHORT;
//   lp_loop.LpAmpCfg.LpTiaRtia = LPTIARTIA_OPEN;
//   lp_loop.LpAmpCfg.LpTiaSW = LPTIASW(7) | LPTIASW(5) | LPTIASW(9);
//   AD5940_LPLoopCfgS(&lp_loop);

//   AD5940_AFECtrlS(AFECTRL_HPREFPWR | AFECTRL_INAMPPWR | AFECTRL_EXTBUFPWR |
//                       AFECTRL_WG | AFECTRL_DACREFPWR | AFECTRL_HSDACPWR,
//                   bTRUE);
//   ESP_LOGI(TAG, "CE0 sine start(BAT-like): f=%.2fHz, offset=%.1fmV(LPDAC=%lu), amp=%.1fmVpp(word=%lu), loopback=%s",
//            freqHz, (double)offMv, (unsigned long)lpdacCode, (double)ampMvpp, (unsigned long)ampWord,
//            withLoopback ? "on" : "off");
// }
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
void changeAD5940ToMeasurement(bool bChange)
{
  if(bChange)
  {
  AppBATCfg.SinFreq = 190000.0f;
  AppBATCfg.ACVoltPP = ACVOLTPP_MEASURE;
  AppBATCfg.DCVolt = DCVOLT_MEASURE;							/* DC 최소전압*/ 
  AppBATCfg.bParaChanged = bTRUE;
    if (AD5940ERR_OK != AppBATInit(AppBuff, APPBUFF_SIZE))
    {
        ESP_LOGW(TAG, "Wakeup Error..retry...");
    }
  }
  else
  {
    AppBATCtrl(BATCTRL_STOPNOW, 0);
    AD5940BATStructInit(); /* SinFreq=1kHz, AC/DC 기본값 + bParaChanged */
    AD5940Err err = AppBATInit(AppBuff, APPBUFF_SIZE);
    ESP_LOGD(TAG, "measurement OFF: SinFreq=%.1f AppBATInit=%d",
             AppBATCfg.SinFreq, (int)err);
    if (err != AD5940ERR_OK)
    {
      ESP_LOGW(TAG, "measurement OFF: AppBATInit failed, WG may stay at 190kHz");
    }
  }
}
float AD5940_calibration(float *real , float *image)
{
  const uint16_t samplesUsed = CAL_TOTAL_SAMPLES - CAL_SKIP_SAMPLES;
  uint16_t loopCount = CAL_TOTAL_SAMPLES;
  uint16_t sampleIndex = 0;
  uint16_t averagedCount = 0;

  AD5940PlatformCfg();
  AD5940BATStructInit();             /* Configure your parameters in this function */
  AppBATInit(AppBuff, APPBUFF_SIZE); /* Initialize BAT application. Provide a buffer, which is used to store sequencer commands */
  *real = 0.0f;
  *image = 0.0f;
  simpleCli.outputStream->printf("Now on calibration(...");
  while (loopCount--)
  {
    time_t startTime = millis();
    if (AD5940ERR_WAKEUP == AppBATCtrl(BATCTRL_MRCAL, 0))
    {
      simpleCli.outputStream->printf("\nWakeup Error..retry...");
    }; /* Measur RCAL each point in sweep */
    time_t endTime = millis();
    const bool useSample = (sampleIndex >= CAL_SKIP_SAMPLES);
    simpleCli.outputStream->printf("\r\n%u%s: R I Mag:%6.2f\t %6.2f\t %6.2f (%dms)",
                           sampleIndex,
                           useSample ? "*" : " ",
                           AppBATCfg.RcalVolt.Real,
                           AppBATCfg.RcalVolt.Image,
                           AD5940_ComplexMag(&AppBATCfg.RcalVolt), (int)(endTime - startTime));
    if (useSample)
    {
      *real += AppBATCfg.RcalVolt.Real;
      *image += AppBATCfg.RcalVolt.Image;
      averagedCount++;
    }
    sampleIndex++;
    delay(100);
  }

  if (averagedCount == 0)
  {
    ESP_LOGW(TAG, "calibration: no samples averaged");
    return 0.0f;
  }

  *real /= (float)averagedCount;
  *image /= (float)averagedCount;
  AppBATCfg.RcalVolt.Real = *real;
  AppBATCfg.RcalVolt.Image = *image;
  ESP_LOGI(TAG, "calibration avg(%u/%u): R=%.3f I=%.3f Mag=%.3f mOhm",
           averagedCount, CAL_TOTAL_SAMPLES, *real, *image,
           AD5940_ComplexMag(&AppBATCfg.RcalVolt));
  return AD5940_ComplexMag(&AppBATCfg.RcalVolt);
}
void AD5940_Main(void *parameters);
void AD5940_init(){
  AD5940_MCUResourceInit(0);
  AD5940_Main_init();
  ESP_LOGI(TAG, "Chip Id : %d\n", AD5940_ReadReg(REG_AFECON_CHIPID));
  //AD5940_ShutDown();
  //xTaskCreate(AD5940_Main, "AD5940_Main", 5000, NULL, 1, NULL);
}
float AD5940_readImpMagnitude()
{

  AppBATInit(AppBuff, APPBUFF_SIZE); /* Initialize BAT application. Provide a buffer, which is used to store sequencer commands */

  time_t startTime = millis();
  if (AD5940ERR_WAKEUP == AppBATCtrl(BATCTRL_START, 0))
  {
    ESP_LOGW(TAG, "readImp: BATCTRL_START wakeup failed");
    return 0.0f;
  }

  while (AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH) == bFALSE)
  {
    delay(10);
    if (millis() - startTime > 8000)
    {
      ESP_LOGW(TAG, "readImp: timeout %dms", (int)(millis() - startTime));
      AppBATCtrl(BATCTRL_STOPNOW, 0);
      return 0.0f;
    }
  }

  ESP_LOGD(TAG, "readImp: FIFO ready (%dms)", (int)(millis() - startTime));
  AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
  AD5940_ClrMCUIntFlag();
  uint32_t temp = APPBUFF_SIZE;
  AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH, bTRUE);
  AppBATISR(AppBuff, &temp);

  if (temp == 0)
  {
    ESP_LOGW(TAG, "readImp: AppBATISR returned no battery data");
    return 0.0f;
  }

  BATShowResult(AppBuff, temp);
  fImpCar_Type *pImp = (fImpCar_Type *)AppBuff;
  return AD5940_ComplexMag(&pImp[0]);
}

void AD5940_Main(void *parameters)
{
  AD5940_init();
  uint32_t temp;
  outputStream  = &Serial;//static_cast<Print *>(parameters);

  AppBATCfg.RcalVolt.Real = systemDefaultValue.real_Cal;
  AppBATCfg.RcalVolt.Image = systemDefaultValue.image_Cal; 
  uint16_t loopCount=0 ;

  float real , image;
  float ImpMagnitude = AD5940_calibration(&real,&image);

  AppBATCtrl(BATCTRL_START, 0);
  while(1){

    if(loopCount == MAX_LOOP_COUNT-1)loopCount =0;
    time_t startTime = millis();
    esp_task_wdt_reset();
    uint32_t timeout = 100000; // 적절한 카운트 설정
    while(AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH) == bFALSE) {
      timeout--;
      if(timeout == 0) {
        // 에러 처리 로직 (예: 시스템 리셋 또는 에러 메시지 출력)
        ESP_LOGW(TAG, "Time out reached %d", millis() - startTime);
        break;
      }
    }
    // while(!AD5940_GetMCUIntFlag())
    // {
    //   delay(100);
    //   if (millis() - startTime > 3000)
    //   {
    //     ESP_LOGW(TAG, "Time out reached %d", millis() - startTime);
    //     break; 
    //   };
    //   //ESP_LOGW(TAG, "retry %d", millis() - startTime);
    // }
    ESP_LOGW(TAG, "Interrupt Occured(%dms)", millis() - startTime);
    //if(AD5940_GetMCUIntFlag())
    {
      AD5940_AGPIOToggle(AGPIO_Pin1); // LED ON OFF
      AD5940_INTCClrFlag(AFEINTSRC_ALLINT);
      AD5940_ClrMCUIntFlag(); /* Clear this flag */
      temp = APPBUFF_SIZE;
      AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH, bTRUE);
      AppBATISR(AppBuff, &temp); /* Deal with it and provide a buffer to store data we got */
      delay(100);
      // AD5940_Delay10us(100000);
      addResult(AppBuff, loopCount);
      BATShowResult(AppBuff, temp); /* Print measurement results over UART */
      AD5940_SEQMmrTrig(SEQID_0);   /* Trigger next measurement ussing MMR write*/
    }
    loopCount++;
    delay(1000);
  }

  //for(loopCount =0;loopCount < MAX_LOOP_COUNT ;loopCount++ )
  printf("----AD5940_Main start----\n");
  for(;;)
  {
    /* Check if interrupt flag which will be set when interrupt occurred. */
    //esp_task_wdt_reset();

    // while (AD5940_INTCTestFlag(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH) == bFALSE)
    // {
    //   if( millis()-startTime > 1000){ESP_LOGW(TAG, "Time out reached %d",millis()-startTime);break;} 
    // } ;

    //while(!AD5940_GetMCUIntFlag())
  }
}

/**
 * @}
 * @}
 * */
