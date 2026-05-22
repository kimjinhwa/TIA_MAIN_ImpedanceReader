#define VERSION "1.0.8" // Modbus 485 통신 기능 테스트 완료 
//#define VERSION "1.0.7" // CT 전류 측정 까지 실측하여 정확하게 보정함. 
//#define VERSION "1.0.6" // NTC 측정 추가.  
//#define VERSION "1.0.5" // 임피던스 측정 성공 시 EEPROM 갱신 조건 변경.  
// 기존 조건: 기존 대비 **5% 이상 증가** 시만 (노이즈·측정오차 여유).
// 새 조건: 기존 대비 **5% 이상 변화**(증가·감소) 시만 (셀 교체·결선 수정 등 반영).
// 이전 버전에서 사용한 증가 조건은 충전 중 변동 대비 부족했음.
//#define VERSION "1.0.4" // 메인 루프에서 15개의 셀을 순환하는 루틴까지 완성함.  
//#define VERSION "1.0.4" // 파형을 멈추고 전압을 읽는 루틴을 완성함.  
// pBATCfg->bParaChanged = bTRUE; 을 사용하여 파형을 멈추고 전압을 읽는 루틴을 완성함.  
// AppBATInit()이 시퀀서(SinFreq/WG)를 SRAM에 다시 쓰도록 함. 
//#define VERSION "1.0.4" // 파형을 멈추고 전압을 읽는 루틴을 완성함.  
// pBATCfg->bParaChanged = bTRUE; 을 사용하여 파형을 멈추고 전압을 읽는 루틴을 완성함.  
// AppBATInit()이 시퀀서(SinFreq/WG)를 SRAM에 다시 쓰도록 함. 
//#define VERSION "1.0.4" // 파형을 멈추고 전압을 읽는 루틴을 완성함.  
// pBATCfg->bParaChanged = bTRUE; 을 사용하여 파형을 멈추고 전압을 읽는 루틴을 완성함.  
// AppBATInit()이 시퀀서(SinFreq/WG)를 SRAM에 다시 쓰도록 함. 
//#define VERSION "1.0.4" // 파형을 멈추고 전압을 읽는 루틴을 완성함.  
// pBATCfg->bParaChanged = bTRUE; 을 사용하여 파형을 멈추고 전압을 읽는 루틴을 완성함.  
// AppBATInit()이 시퀀서(SinFreq/WG)를 SRAM에 다시 쓰도록 함. 
//efine VERSION "1.0.3" // 파형을 멈추고 전압을 읽는 루틴을 완성함.  
//#define VERSION "1.0.3" // EEPROM 레이아웃 변경 
//#define VERSION "1.0.3" // 전압 튜닝을 완료함. 
//16개의 샘플을 읽을때 약 860ms이 걸린다. 32개의 샘플을 읽을때 약 1669ms이 걸린다. 
//정확도는 비슷하다.
// 더미 저항 100Kohm을 사용하여 전압을 튜닝함. 
//#define VERSION "1.0.3" // 모드버스 Address 읽기 완료 
// AD5940_ISR의 번지수 충돌의 버그를 찾았다.
//#define VERSION "1.0.2" // 전압 , 전류 루틴까지 완료한다. 
//#define VERSION "1.0.1" // 전압은 완벽하게 읽힌다. 
//전압을 읽을때 16개의 샘플은 약 860ms이 걸린다.
//#define VERSION "1.0.0" // AD5941 통신 OK 
//branch mainDev4.0 에서 작업한다. 