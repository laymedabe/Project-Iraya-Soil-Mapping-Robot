#include "npk_sensor.h"
#include "config.h"
#include <SoftwareSerial.h>

SoftwareSerial npkSerial(RS485_RX, RS485_TX);

// Modbus RTU Command: Requesting 7 registers starting at address 0x0000
const byte allParamsRequestFrame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08};
byte allParamsResponseFrame[19]; // 3 header + 14 data + 2 CRC = 19 bytes

void npkInit() {
  npkSerial.begin(4800); 
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW); // Receive mode by default
}

void readNPKSensor() {
  // Clear incoming serial buffer
  while (npkSerial.available()) { npkSerial.read(); }

  // Switch MAX485 to Transmit mode
  digitalWrite(RS485_DE_RE, HIGH);
  delay(10);

  // Send Modbus Request Frame
  npkSerial.write(allParamsRequestFrame, sizeof(allParamsRequestFrame));
  npkSerial.flush();

  // Switch MAX485 back to Receive mode
  digitalWrite(RS485_DE_RE, LOW);
  delay(10);

  // Wait for Modbus Response
  unsigned long timeout = millis();
  byte index = 0;

  while ((millis() - timeout < 500) && (index < sizeof(allParamsResponseFrame))) {
    if (npkSerial.available()) {
      allParamsResponseFrame[index++] = npkSerial.read();
    }
  }

  // Process data if valid frame received (19 bytes expected)
  if (index >= 19) {
    uint16_t nitrogen =  (allParamsResponseFrame[11] << 8) | allParamsResponseFrame[12];
    uint16_t phos     =  (allParamsResponseFrame[13] << 8) | allParamsResponseFrame[14];
    uint16_t potass   =  (allParamsResponseFrame[15] << 8) | allParamsResponseFrame[16];

    // Forward the data to the Raspberry Pi over the main Serial connection
    // Format: DATA NPK <N> <P> <K>
    PI_SERIAL.print(F("DATA NPK "));
    PI_SERIAL.print(nitrogen);
    PI_SERIAL.print(F(" "));
    PI_SERIAL.print(phos);
    PI_SERIAL.print(F(" "));
    PI_SERIAL.println(potass);
  } else {
    PI_SERIAL.println(F("FAULT NPK_TIMEOUT"));
  }
}
