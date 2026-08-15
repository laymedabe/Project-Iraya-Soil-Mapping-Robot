#ifndef IRAYA_NPK_SENSOR_H
#define IRAYA_NPK_SENSOR_H

#include <Arduino.h>

struct NpkReading {
  float nitrogen;     // mg/kg (from Modbus register)
  float phosphorus;   // mg/kg (from Modbus register)
  bool valid;
  // Raw hex response stored for CSV logging (matches rccode.ino output)
  uint8_t rawResponse[32];
  int rawLength;
};

void npkInit();

// Sends the Modbus RTU "Read Holding Registers" command matching the
// real sensor (0x01 0x03 0x00 0x00 0x00 0x02 0xC4 0x0B) and blocks
// briefly (~1s timeout) for the response. Returns a reading with
// valid=false if the sensor didn't respond.
NpkReading npkRead();

// Prints raw hex bytes to PI_SERIAL (matches rccode.ino CSV output format)
void npkPrintRawHex(const NpkReading& reading);

#endif
