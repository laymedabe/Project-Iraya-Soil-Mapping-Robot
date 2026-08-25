#ifndef NPK_SENSOR_H
#define NPK_SENSOR_H

#include <Arduino.h>

// Initializes the SoftwareSerial and DE/RE pin for the NPK sensor
void npkInit();

// Sends Modbus request and reads the response, then prints DATA NPK over PI_SERIAL
void readNPKSensor();

#endif
