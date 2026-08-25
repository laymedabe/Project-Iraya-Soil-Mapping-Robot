#include "actuator_control.h"
#include "config.h"
#include "npk_sensor.h"

static ActuatorState actState = ACT_IDLE;
static unsigned long actTimer = 0;

void actuatorInit() {
  pinMode(RELAY_ACT_1, OUTPUT);
  pinMode(RELAY_ACT_2, OUTPUT);
  stopActuatorRelay();
}

void startActuatorExtend() {
  actState = ACT_EXTENDING;
  actTimer = millis();

  // Extend Actuator (Active-LOW Relays)
  // Lead 1 → +12V (relay ON = LOW), Lead 2 → GND (relay OFF = HIGH)
  digitalWrite(RELAY_ACT_1, LOW);
  digitalWrite(RELAY_ACT_2, HIGH);
}

void updateActuatorSequence() {
  switch (actState) {

    case ACT_EXTENDING:
      if (millis() - actTimer >= ACTUATOR_EXTEND_TIME_MS) {
        PI_SERIAL.println(F(">> Probe inserted into soil. Reading NPK & GPS mapping data..."));
        PI_SERIAL.println("STATUS READING");
        stopActuatorRelay();
        actState = ACT_HOLDING;
        actTimer = millis();
      }
      break;

    case ACT_HOLDING:
      // Data capture is handled by the Raspberry Pi during this state
      // (The Pi observes STATUS READING)
      
      // Read the NPK sensor once after 2 seconds to let the probe settle in the soil
      static bool npkReadDone = false;
      if (!npkReadDone && millis() - actTimer >= 2000) {
        readNPKSensor();
        npkReadDone = true;
      }

      // Hold probe extended in soil for the configured duration
      if (millis() - actTimer >= ACTUATOR_HOLD_TIME_MS) {
        PI_SERIAL.println(F(">> Sampling complete. Retracting probe..."));
        PI_SERIAL.println("STATUS RETRACTING");
        actState = ACT_RETRACTING;
        actTimer = millis();
        npkReadDone = false; // Reset for next cycle

        // Retract Actuator (Active-LOW Relays)
        // Lead 1 → GND (relay OFF = HIGH), Lead 2 → +12V (relay ON = LOW)
        digitalWrite(RELAY_ACT_1, HIGH);
        digitalWrite(RELAY_ACT_2, LOW);
      }
      break;

    case ACT_RETRACTING:
      if (millis() - actTimer >= ACTUATOR_RETRACT_TIME_MS) {
        PI_SERIAL.println(F(">> Probe fully retracted. Robot ready to navigate."));
        PI_SERIAL.println("STATUS IDLE");
        PI_SERIAL.println(F("----------------------------------------------------------\n"));
        stopActuatorRelay();
        actState = ACT_IDLE;
      }
      break;

    case ACT_IDLE:
    default:
      break;
  }
}

void stopActuatorRelay() {
  // Active-LOW Relays turn OFF when driven HIGH
  digitalWrite(RELAY_ACT_1, HIGH);
  digitalWrite(RELAY_ACT_2, HIGH);
}

ActuatorState getActuatorState() {
  return actState;
}

const char* getActuatorStateLabel() {
  switch (actState) {
    case ACT_EXTENDING:  return "LOWERING";
    case ACT_HOLDING:    return "READING";
    case ACT_RETRACTING: return "RAISED";
    case ACT_IDLE:
    default:             return "IDLE";
  }
}
