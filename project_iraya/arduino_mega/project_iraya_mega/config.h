#ifndef IRAYA_CONFIG_H
#define IRAYA_CONFIG_H

/*
 * Pin map — UPDATE THESE to match your actual wiring before flashing.
 * Assumes a 4-channel motor driver (e.g. BTS7960 or similar dual-H-bridge
 * modules, one pair per side) and a linear actuator driven through a
 * separate H-bridge channel.
 */

// ---- Drive motors (skid-steer: left pair, right pair) ----
#define LEFT_PWM_PIN    5
#define LEFT_DIR_PIN    4
#define RIGHT_PWM_PIN   6
#define RIGHT_DIR_PIN   7

// ---- Linear actuator (NPK sensor insertion) ----
#define ACTUATOR_PWM_PIN   8
#define ACTUATOR_DIR_PIN   9
#define ACTUATOR_POT_PIN   A0   // potentiometer/linear position feedback (analog)
#define ACTUATOR_CURRENT_PIN A1 // current sense (for stall detection), if available

// ---- RS485 NPK sensor (Modbus RTU over SoftwareSerial or Serial2) ----
#define RS485_DE_RE_PIN  22     // driver enable / receiver enable, tied together
#define RS485_SERIAL     Serial2
#define NPK_SLAVE_ID     0x01

// ---- Physical E-STOP input (also wired directly into motor driver enable — this pin is just for status reporting) ----
#define ESTOP_PIN        2      // interrupt-capable pin

// ---- Communication with Raspberry Pi ----
#define PI_SERIAL        Serial   // USB-CDC / UART0 to the Pi
#define PI_BAUD          115200

// ---- Safety tuning ----
#define DRIVE_WATCHDOG_TIMEOUT_MS   400
#define ACTUATOR_MAX_TRAVEL_TIME_MS 4000   // abort if insertion/retraction takes longer than this
#define ACTUATOR_STALL_CURRENT_MA   1500   // trip threshold, tune to your actuator's datasheet
#define ACTUATOR_TARGET_DEPTH_MM    130
#define ACTUATOR_MAX_STROKE_MM      150

#endif
