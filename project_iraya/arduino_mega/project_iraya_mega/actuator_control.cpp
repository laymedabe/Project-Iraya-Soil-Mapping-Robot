#include "actuator_control.h"
#include "config.h"

void actuatorInit() {
  pinMode(ACTUATOR_PWM_PIN, OUTPUT);
  pinMode(ACTUATOR_DIR_PIN, OUTPUT);
  pinMode(ACTUATOR_POT_PIN, INPUT);
  pinMode(ACTUATOR_CURRENT_PIN, INPUT);
  analogWrite(ACTUATOR_PWM_PIN, 0);
}

// Converts a raw potentiometer ADC reading (0-1023) to mm of travel.
// CALIBRATE these two constants against your actual actuator + pot before
// relying on this for anything beyond rough position feedback.
static int potToMm(int raw) {
  const int POT_AT_ZERO_MM = 60;     // ADC value when fully retracted
  const int POT_AT_MAX_MM = 900;     // ADC value when fully extended (150mm)
  long mm = map(raw, POT_AT_ZERO_MM, POT_AT_MAX_MM, 0, ACTUATOR_MAX_STROKE_MM);
  return constrain(mm, 0, ACTUATOR_MAX_STROKE_MM);
}

static int readCurrentMa() {
  // Placeholder linear mapping — replace with your current sensor's actual
  // datasheet conversion (e.g. ACS712: (raw - zeroOffset) * sensitivity).
  int raw = analogRead(ACTUATOR_CURRENT_PIN);
  return raw * 5; // rough placeholder scale, TUNE THIS
}

int actuatorCurrentDepthMm() {
  return potToMm(analogRead(ACTUATOR_POT_PIN));
}

static bool driveToDepth(int targetMm, bool extending) {
  unsigned long start = millis();
  digitalWrite(ACTUATOR_DIR_PIN, extending ? HIGH : LOW);
  analogWrite(ACTUATOR_PWM_PIN, 200); // fixed speed; swap for PID if needed

  while (true) {
    int depth = actuatorCurrentDepthMm();
    bool reached = extending ? (depth >= targetMm) : (depth <= targetMm);
    if (reached) break;

    if (readCurrentMa() > ACTUATOR_STALL_CURRENT_MA) {
      analogWrite(ACTUATOR_PWM_PIN, 0);
      PI_SERIAL.println("FAULT ACTUATOR_STALL");
      return false;
    }
    if (millis() - start > ACTUATOR_MAX_TRAVEL_TIME_MS) {
      analogWrite(ACTUATOR_PWM_PIN, 0);
      PI_SERIAL.println("FAULT ACTUATOR_TIMEOUT");
      return false;
    }
    delay(5);
  }

  analogWrite(ACTUATOR_PWM_PIN, 0);
  return true;
}

bool actuatorLower() {
  return driveToDepth(ACTUATOR_TARGET_DEPTH_MM, true);
}

bool actuatorRaise() {
  return driveToDepth(0, false);
}
