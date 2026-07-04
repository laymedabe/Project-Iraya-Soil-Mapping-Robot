#ifndef IRAYA_ACTUATOR_CONTROL_H
#define IRAYA_ACTUATOR_CONTROL_H

#include <Arduino.h>

void actuatorInit();

// Blocking calls are intentional here: the Mega's main loop is otherwise
// idle during a sample cycle (drive is stopped, sensor read is fast), and
// blocking keeps the state machine trivially easy to reason about and test.
// Each call enforces ACTUATOR_MAX_TRAVEL_TIME_MS and current-based stall
// detection internally, and reports FAULT lines if either trips.
bool actuatorLower();   // returns false if a fault occurred
bool actuatorRaise();   // returns false if a fault occurred

int actuatorCurrentDepthMm();

#endif
