// =================================================================================================
// eModbus: Copyright 2020 by Michael Harwerth, Bert Melis and the contributors to ModbusClient
//               MIT license - see license.md for details
// =================================================================================================
// Includes: <Arduino.h> for Serial etc., WiFi.h for WiFi support
#include <Arduino.h>
#include "HardwareSerial.h"

#include "ModbusServerRTU.h"


void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  Serial.println("__ OK __");

}

void loop() {
  delay(10000);
}
