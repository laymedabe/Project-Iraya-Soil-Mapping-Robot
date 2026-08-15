#include "npk_sensor.h"
#include "config.h"

/*
 * Modbus RTU driver for the RS485 NPK soil sensor.
 *
 * The request frame matches the real rccode.ino exactly:
 *   {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B}
 *   Slave 0x01, Function 0x03 (Read Holding Registers),
 *   Start register 0x0000, Quantity 0x0002, CRC 0x0BC4
 *
 * Expected response: 9 bytes
 *   [SlaveID][0x03][ByteCount=4][Reg0_Hi][Reg0_Lo][Reg1_Hi][Reg1_Lo][CRC_Lo][CRC_Hi]
 *   Register 0 → Nitrogen (mg/kg)
 *   Register 1 → Phosphorus (mg/kg)
 */

// Pre-computed Modbus RTU request frame (matches rccode.ino)
static const byte sensorRequest[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B};

void npkInit() {
  pinMode(DE_RE_PIN, OUTPUT);
  digitalWrite(DE_RE_PIN, LOW);     // Start in receive mode
  RS485_SERIAL.begin(NPK_BAUD);    // 9600 baud, matching real hardware
}

NpkReading npkRead() {
  NpkReading result = {0, 0, false, {0}, 0};

  // 1. Switch MAX485 to TRANSMIT mode
  digitalWrite(DE_RE_PIN, HIGH);
  delay(10);

  // 2. Transmit Modbus command frame
  RS485_SERIAL.write(sensorRequest, sizeof(sensorRequest));
  RS485_SERIAL.flush();

  // 3. Switch MAX485 back to RECEIVE mode
  digitalWrite(DE_RE_PIN, LOW);

  // 4. Read response frame with 1-second timeout (matches rccode.ino)
  unsigned long startTime = millis();
  bool responseReceived = false;
  int received = 0;

  while (millis() - startTime < 1000) {
    if (RS485_SERIAL.available()) {
      byte incomingByte = RS485_SERIAL.read();
      if (received < (int)sizeof(result.rawResponse)) {
        result.rawResponse[received] = incomingByte;
      }
      received++;
      responseReceived = true;
    }
  }

  result.rawLength = received;

  if (!responseReceived) {
    return result;  // Timeout — sensor didn't respond
  }

  // Parse the response if we got the expected 9 bytes:
  // [SlaveID][FuncCode][ByteCount][Reg0_Hi][Reg0_Lo][Reg1_Hi][Reg1_Lo][CRC_Lo][CRC_Hi]
  if (received >= 9 && result.rawResponse[1] == 0x03 && result.rawResponse[2] == 0x04) {
    result.nitrogen   = (float)((result.rawResponse[3] << 8) | result.rawResponse[4]);
    result.phosphorus = (float)((result.rawResponse[5] << 8) | result.rawResponse[6]);
    result.valid = true;
  }

  return result;
}

void npkPrintRawHex(const NpkReading& reading) {
  if (reading.rawLength == 0) {
    PI_SERIAL.print(F("[ERROR_TIMEOUT]"));
  } else {
    for (int i = 0; i < reading.rawLength; i++) {
      if (reading.rawResponse[i] < 0x10) PI_SERIAL.print("0");
      PI_SERIAL.print(reading.rawResponse[i], HEX);
      PI_SERIAL.print(" ");
    }
  }
}
