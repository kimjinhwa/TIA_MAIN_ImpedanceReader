
```
 SPI.setFrequency(spiClk);
  SPI.begin(SCK, MISO, MOSI, CS_5940);
  pinMode(SS, OUTPUT); // VSPI SS -> 아니다..이것은 리셋용이다.
#define MISO                GPIO_NUM_12  
#define MOSI                GPIO_NUM_13  
#define SCK                 GPIO_NUM_14  
                
#define AD5940_ISR          GPIO_NUM_32  
#define CS_5940             GPIO_NUM_15  
static const int spiClk = 1000000; // 1 MHz

SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

```

