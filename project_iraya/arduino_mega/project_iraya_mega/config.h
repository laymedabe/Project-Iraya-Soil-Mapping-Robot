#ifndef OROSA_CONFIG_H
#define OROSA_CONFIG_H

/*
 * Pin map — matches the real hardware wiring from the working rccode.ino.
 * Arduino Mega 2560 with dual BTS7960 motor drivers, 2-channel relay
 * for linear actuator, MAX485 RS485 NPK sensor, NEO-M8 GPS, and IR remote.
 */
#include <Arduino.h>

// ---- IR Receiver ----
#define IR_RECEIVE_PIN  11

// ---- Left Motor Driver (BTS7960 #1) ----
#define L_RPWM  5      // Forward PWM
#define L_LPWM  6      // Reverse PWM
#define L_R_EN  7      // Right-enable
#define L_L_EN  8      // Left-enable

// ---- Right Motor Driver (BTS7960 #2) ----
// Pins 44 & 45 avoid Timer 2 PWM conflicts with IRremote library
#define R_RPWM  44     // Forward PWM
#define R_LPWM  45     // Reverse PWM
#define R_R_EN  4      // Right-enable
#define R_L_EN  2      // Left-enable

// ---- Linear Actuator (2-Channel Power Relay, Active-LOW) ----
#define RELAY_ACT_1  22   // Controls Actuator Lead 1
#define RELAY_ACT_2  23   // Controls Actuator Lead 2



// ---- Physical E-STOP input (also wired directly into motor driver enable — this pin is just for status reporting) ----
#define ESTOP_PIN       3      // interrupt-capable pin (pin 2 is used by R_L_EN)

// ---- Communication with Raspberry Pi ----
#define PI_SERIAL       Serial   // USB-CDC / UART0 to the Pi
#define PI_BAUD         9600     // Matches working rccode.ino

// ---- Speed & Timing Parameters ----
#define MOTOR_SPEED              200    // Default PWM speed (0 to 255)
#define ACTUATOR_EXTEND_TIME_MS  3000   // Actuator extend duration
#define ACTUATOR_HOLD_TIME_MS    5000   // Time probe stays in soil
#define ACTUATOR_RETRACT_TIME_MS 3000   // Actuator retract duration

// ---- Safety tuning ----
#define DRIVE_WATCHDOG_TIMEOUT_MS  400  // Stop if no drive command in this window

// ---- Auto-cycle timing ----
#define AUTO_DRIVE_INTERVAL_MS   10000  // Drive for 10s between each sample stop

// ---- IR Remote HEX Commands ----
#define IR_FORWARD   0xB946FF00
#define IR_BACKWARD  0xEA15FF00
#define IR_LEFT      0xBB44FF00
#define IR_RIGHT     0xBC43FF00
#define IR_STOP      0xBF40FF00
#define IR_ACTUATE   0xF708FF00   // Triggers Actuator + GPS + NPK

#endif
