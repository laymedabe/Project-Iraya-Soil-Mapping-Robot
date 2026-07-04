#ifndef IRAYA_NPK_SENSOR_H
#define IRAYA_NPK_SENSOR_H

#include <Arduino.h>

struct NpkReading {
  float nitrogen;     // mg/kg
  float phosphorus;   // mg/kg
  float potassium;    // mg/kg
  float moisture;     // %
  float temperature;  // deg C
  float ec;           // dS/m
  bool valid;
};

void npkInit();

// Sends a Modbus RTU "Read Holding Registers" request and blocks briefly
// (~50-100ms) for the response. Returns a reading with valid=false if the
// sensor didn't respond or the CRC check failed.
NpkReading npkRead();

#endif
