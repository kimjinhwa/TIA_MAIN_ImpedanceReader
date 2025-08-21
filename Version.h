#define VERSION "0.9.8"  // multicasting  부분 변경 
//#define VERSION "0.9.7"  // 가장높은 온도센서 2개만 서버에 올려준다>  
//#define VERSION "0...2"  // 온도 센서 추가 TH3, TH4, IN_TH 
//#define VERSION "0.9.1"  // Add Modbus Address rutine using ASEL1, ASEL2, ASEL3 
//#define VERSION "0.9.0"  // make cell balance and add ASEL1, ASEL2, ASEL3 
//#define VERSION "1.3.5"  // Hole CT값을 CT Ratio로 사용하기 위하여 변경한다.  CT용량값을 정수로 기입한다.
//#define VERSION "1.3.4"  // platio 수정 
//#define VERSION "1.3.3"  // Total gain을 따로 준다. 
//#define VERSION "1.3.2"  // 전압을 읽을때 1번이상을 버려야 정확한 값을 읽는다. 
//#define VERSION "1.3.1"  // 전류 필터링을 바꾼다.
//efine VERSION "1.3.0"  // Open Wire 를 제거한다. 1.2V에는 맞지 않는 다.
//#define VERSION "1.2.9"  // 전류 읽는 부분을 강화 하였다. 
//#define VERSION "1.2.8"  // Version 프린팅을 추가하였다. 
//#define VERSION "1.2.8"  // 전류 오프셋이 unit 로 되어 있어서 int로 수정함.
//#define VERSION "1.2.7"  // 전류 Marking 해제. 
//#define VERSION "1.2.6"  // 전류 오프셋 보정 기능 추가
//#define VERSION "1.2.5"  // 부팅시간을 각 모듈마다 다르게 한다. 
//#define VERSION "1.2.4"  // 전압에 필터링 100개를 구현한다. 
//#define VERSION "1.2.3"  // OFFSET 0으로 변경 , Gain 1220으로 변경
//#define VERSION "1.2.2"  // Ampoere의 2.0V 오프셋 보정 1.914V로 변경  and Gain 1000으로 변경
//#define VERSION "1.2.1"  // Ampere의 기본 offset을 변경 
//#define VERSION "1.2.0"  // Watchdog 오류 해결 
//#define VERSION "1.1.8"  // 시간을 늘인다. 
//#define VERSION "1.1.7"  // watchdog reset 오류 해결 
//#define VERSION "1.1.5"  // Modbus Multicasting 기능을 추가한다. 
//#define VERSION "1.1.4"  // AmpereGain 추가 및 센서 교체로인한 오프셋 보정 추가
//#define VERSION "1.1.3"  // Offset 실수로 전체 전압에 문제가 있었다.  
//#define VERSION "1.1.3"  // 양산보드를 제외한 보드에서 동작 완료 확인 한 버전 
//#define VERSION "1.1.3"  // Open Wire 문제 해결 하였다. 
//#define VERSION "1.1.2"  // 또 다시 셀전압의 문제를 잡았다
//#define VERSION "1.1.1"  // 다시 셀전압의 문제를 잡았다
//#define VERSION "1.1.0"  // 보드 불량인줄 알았던 것이 프로그램 오류가 있었네..
//#define VERSION "1.1.0"  // AutoUpdate 기능 추가, Calibration 기능 추가
//#define VERSION "1.0.9"  // useHoleCT 기능 추가 Modbus address 9번
//#define version "SBMS_1.0.8"  // Wifi ssid 삭제 기능 추가
//#define version "SBMS_1.0.7"  //셀 전압의 오류를 해결하였다. 
//#define version "SBMS_1.0.6"  // 셀의 안전화 전압을 위해 5us 추가
//#define version "SBMS_1.0.6"  // mutex를  xSemaphoreCreateMutex() 로 변경해서  Soleve System Fault
//#define version "SBMS_1.0.5"  // mutex를 사용해 데이타 경쟁 문제를 해결함.
//#define version "SBMS_1.0.4"  // 전류 센싱 부분을 투닝하였다. 다음엔 보간법 알고리즘을 적용해 보자 
//#define version "SBMS_1.0.3"  // Modid를 변경시 즉시 재 부팅하여 새로운 주소로 설정되도록 함. 
//#define version "SBMS_1.0.3"  // 버전관리를 모드버스의 데이타에서 볼 수 있게 함. 
//#define version "SBMS_1.0.2"  // 한글깨짐 문제를 해결해 보자.
// #define version "SBMS_1.0.1"  // 버전관리를 시작함
// #define version "SBMS_1.0.0"  // Release Version 1.0.0
