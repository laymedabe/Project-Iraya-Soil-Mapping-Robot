#include <IRremote.hpp>
#include <SoftwareSerial.h>

// =========================================================================
// PIN DEFINITIONS & SYSTEM CONFIGURATION (Arduino Uno)
// =========================================================================

// IR Receiver Pin
const int IR_RECEIVE_PIN = 2;

// Left Motor Driver (BTS7960 #1) - Timer 0 PWM
const int L_RPWM = 5;
const int L_LPWM = 6;
const int L_R_EN = 7;
const int L_L_EN = 8;

// Right Motor Driver (BTS7960 #2) - Timer 1 PWM (Avoids Timer 2 conflicts with
// IRremote)
const int R_RPWM = 9;
const int R_LPWM = 10;
const int R_R_EN = 4;
const int R_L_EN = 12;

// Linear Actuator 2-Channel Power Relay Module
const int RELAY_ACT_1 = A0; // Controls Actuator Lead 1
const int RELAY_ACT_2 = A1; // Controls Actuator Lead 2

// MAX485 TTL RS485 Module Pins for 7-in-1 Sensor
const int RS485_RX = A2;    // Arduino RX <- MAX485 RO
const int RS485_TX = A3;    // Arduino TX -> MAX485 DI
const int RS485_DE_RE = A4; // RS485 Flow Control (DE and RE tied together)

SoftwareSerial npkSerial(RS485_RX, RS485_TX);

// --- MODBUS RTU COMMAND FRAMES ---
// Frame 1: Query 7 registers at address 0x0000 (Moisture, Temp, EC, pH, Reg4,
// Reg5, Reg6)
const byte allParamsRequestFrame[] = {0x01, 0x03, 0x00, 0x00,
                                      0x00, 0x07, 0x04, 0x08};
byte allParamsResponseFrame[19]; // 3 header + 14 data + 2 CRC = 19 bytes

// Frame 2: Query 3 registers at address 0x001E (Alternate NPK location on
// JXCT/CWT sensors)
const byte npkRequestFrame[] = {0x01, 0x03, 0x00, 0x1E, 0x00, 0x03, 0x65, 0xCD};
byte npkResponseFrame[11]; // 3 header + 6 data + 2 CRC = 11 bytes

// =========================================================================
// SPEED & TIMING PARAMETERS
// =========================================================================

const int MOTOR_SPEED = 200;             // Drivetrain PWM Speed (0 to 255)
const unsigned long EXTEND_TIME = 7000;  // Actuator extend duration (ms)
const unsigned long HOLD_TIME = 10000;   // Time probe stays lowered (ms)
const unsigned long RETRACT_TIME = 7000; // Actuator retract duration (ms)
const unsigned long SENSOR_INTERVAL =
    2000; // Read sensor every 2 seconds during hold

// =========================================================================
// IR REMOTE HEX COMMANDS
// =========================================================================

const unsigned long IR_FORWARD = 0xBC43FF00;
const unsigned long IR_BACKWARD = 0xBB44FF00;
const unsigned long IR_LEFT = 0xB946FF00;
const unsigned long IR_RIGHT = 0xEA15FF00;
const unsigned long IR_STOP = 0xAD52FF00;
const unsigned long IR_ACTUATE =
    0xBF40FF00; // Triggers Linear Actuator Sequence

// =========================================================================
// STATE VARIABLES
// =========================================================================

enum ActuatorState { ACT_IDLE, ACT_EXTENDING, ACT_HOLDING, ACT_RETRACTING };
ActuatorState actState = ACT_IDLE;
unsigned long actTimer = 0;
unsigned long lastSensorReadTimer = 0;

// Function Declarations
void motorForward();
void motorBackward();
void motorLeft();
void motorRight();
void motorStop();
void startActuatorExtend();
void updateActuatorSequence();
void stopActuatorRelay();
void readNPKSensor();

// =========================================================================
// INITIAL SETUP
// =========================================================================

void setup() {
  Serial.begin(9600);
  npkSerial.begin(4800); // 7-in-1 Sensor default baud rate (4800)

  // MAX485 Control Pin Setup
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW); // Receive mode by default

  // Safely set relays off BEFORE declaring outputs to prevent startup relay
  // clicks
  digitalWrite(RELAY_ACT_1, HIGH);
  digitalWrite(RELAY_ACT_2, HIGH);
  pinMode(RELAY_ACT_1, OUTPUT);
  pinMode(RELAY_ACT_2, OUTPUT);

  // Motor Driver Pin Modes
  pinMode(L_RPWM, OUTPUT);
  pinMode(L_LPWM, OUTPUT);
  pinMode(L_R_EN, OUTPUT);
  pinMode(L_L_EN, OUTPUT);
  pinMode(R_RPWM, OUTPUT);
  pinMode(R_LPWM, OUTPUT);
  pinMode(R_R_EN, OUTPUT);
  pinMode(R_L_EN, OUTPUT);

  // Enable BTS7960 Drivers
  digitalWrite(L_R_EN, HIGH);
  digitalWrite(L_L_EN, HIGH);
  digitalWrite(R_R_EN, HIGH);
  digitalWrite(R_L_EN, HIGH);

  // Start IR Receiver
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  motorStop();

  Serial.println(
      F("=========================================================="));
  Serial.println(
      F("     ARDUINO UNO ROBOT ACTUATOR SYSTEM INITIALIZED        "));
  Serial.println(
      F("=========================================================="));
  Serial.flush();
}

// =========================================================================
// MAIN CONTROL LOOP
// =========================================================================

void loop() {
  // Safety: if actuator just became IDLE but EN pins were left LOW, re-enable them
  if (actState == ACT_IDLE) {
    digitalWrite(L_R_EN, HIGH);
    digitalWrite(L_L_EN, HIGH);
    digitalWrite(R_R_EN, HIGH);
    digitalWrite(R_L_EN, HIGH);

    if (IrReceiver.decode()) {
      unsigned long rawCode = IrReceiver.decodedIRData.decodedRawData;
      uint16_t cmd = IrReceiver.decodedIRData.command;

      if (!(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
        if (rawCode == IR_FORWARD || cmd == 0x43) {
          Serial.println(F("[MOTOR] FORWARD"));
          motorForward();
        } else if (rawCode == IR_BACKWARD || cmd == 0x44) {
          Serial.println(F("[MOTOR] BACKWARD"));
          motorBackward();
        } else if (rawCode == IR_LEFT || cmd == 0x46) {
          Serial.println(F("[MOTOR] LEFT"));
          motorLeft();
        } else if (rawCode == IR_RIGHT || cmd == 0x15) {
          Serial.println(F("[MOTOR] RIGHT"));
          motorRight();
        } else if (rawCode == IR_STOP || cmd == 0x52) {
          Serial.println(F("[MOTOR] STOP"));
          motorStop();
          stopActuatorRelay();
        } else if (rawCode == IR_ACTUATE || cmd == 0x40) {
          Serial.println(F("[MOTOR] ACTUATE SEQUENCE TRIGGERED"));
          motorStop();

          // Disable motor drivers to prevent electrical noise
          digitalWrite(L_R_EN, LOW);
          digitalWrite(L_L_EN, LOW);
          digitalWrite(R_R_EN, LOW);
          digitalWrite(R_L_EN, LOW);

          Serial.println(
              F("\n>> ACTUATOR STARTED: All remote buttons locked for 24s..."));
          Serial.flush();
          delay(80);

          startActuatorExtend();
        } else {
          // Print full IR diagnostic details
          Serial.print(F("[IR] "));
          IrReceiver.printIRResultShort(&Serial);
          Serial.print(F(" | Raw: 0x"));
          Serial.print(rawCode, HEX);
          Serial.print(F(" | Cmd: 0x"));
          Serial.println(cmd, HEX);
          Serial.flush();
        }
      }

      IrReceiver.resume();
    }

    // Serial commands from Raspberry Pi dashboard
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();

      if (cmd == "SAMPLE" || cmd == "ACTUATE") {
        motorStop();
        digitalWrite(L_R_EN, LOW);
        digitalWrite(L_L_EN, LOW);
        digitalWrite(R_R_EN, LOW);
        digitalWrite(R_L_EN, LOW);

        Serial.println(
            F("\n>> ACTUATOR STARTED: Dashboard command received..."));
        Serial.flush();
        delay(80);

        startActuatorExtend();

      } else if (cmd.startsWith("DRIVE ")) {
        // Format: DRIVE FWD 200  /  DRIVE BACK 200  /  DRIVE LEFT 200  /  DRIVE RIGHT 200
        String parts = cmd.substring(6); // strip "DRIVE "
        int spIdx = parts.indexOf(' ');
        String dir = (spIdx >= 0) ? parts.substring(0, spIdx) : parts;
        int spd = (spIdx >= 0) ? parts.substring(spIdx + 1).toInt() : MOTOR_SPEED;
        spd = constrain(spd, 0, 255);

        // Temporarily set speed from command
        if (dir == "FWD")   { analogWrite(L_RPWM, 0); analogWrite(R_RPWM, 0); analogWrite(L_LPWM, spd); analogWrite(R_LPWM, spd); Serial.println(F("[SERIAL] DRIVE FWD")); }
        else if (dir == "BACK")  { analogWrite(L_LPWM, 0); analogWrite(R_LPWM, 0); analogWrite(L_RPWM, spd); analogWrite(R_RPWM, spd); Serial.println(F("[SERIAL] DRIVE BACK")); }
        else if (dir == "LEFT")  { analogWrite(L_LPWM, 0); analogWrite(R_RPWM, 0); analogWrite(L_RPWM, spd); analogWrite(R_LPWM, spd); Serial.println(F("[SERIAL] DRIVE LEFT")); }
        else if (dir == "RIGHT") { analogWrite(R_LPWM, 0); analogWrite(L_RPWM, 0); analogWrite(L_LPWM, spd); analogWrite(R_RPWM, spd); Serial.println(F("[SERIAL] DRIVE RIGHT")); }
        else                     { motorStop(); Serial.println(F("[SERIAL] STOP")); }

      } else if (cmd == "STOP") {
        motorStop();
        Serial.println(F("[SERIAL] STOP"));
      }
    }
  } else {
    if (IrReceiver.decode()) {
      IrReceiver.resume();
    }
  }

  updateActuatorSequence();
}


// =========================================================================
// ACTUATOR AUTOMATION & STATE MACHINE
// =========================================================================

void startActuatorExtend() {
  actState = ACT_EXTENDING;
  actTimer = millis();
  Serial.println(F("STATUS LOWERING"));

  digitalWrite(RELAY_ACT_1, LOW);  // Lead 1 -> +12V
  digitalWrite(RELAY_ACT_2, HIGH); // Lead 2 -> GND
}

void updateActuatorSequence() {
  switch (actState) {

  case ACT_EXTENDING:
    if (millis() - actTimer >= EXTEND_TIME) {
      stopActuatorRelay();
      delay(50);

      Serial.println(F("STATUS READING"));
      Serial.println(F(">> Probe fully lowered (7s elapsed). Starting 10s "
                       "continuous monitoring..."));
      Serial.flush();

      actState = ACT_HOLDING;
      actTimer = millis();
      lastSensorReadTimer = 0;
    }
    break;

  case ACT_HOLDING:
    if (millis() - lastSensorReadTimer >= SENSOR_INTERVAL) {
      lastSensorReadTimer = millis();
      readNPKSensor();
    }

    if (millis() - actTimer >= HOLD_TIME) {
      Serial.println(F("STATUS RETRACTING"));
      Serial.println(
          F(">> Hold duration complete (10s elapsed). Retracting for 7s..."));
      Serial.flush();
      delay(50);

      actState = ACT_RETRACTING;
      actTimer = millis();

      digitalWrite(RELAY_ACT_1, HIGH); // Lead 1 -> GND
      digitalWrite(RELAY_ACT_2, LOW);  // Lead 2 -> +12V
    }
    break;

  case ACT_RETRACTING:
    if (millis() - actTimer >= RETRACT_TIME) {
      stopActuatorRelay();
      delay(50);

      digitalWrite(L_R_EN, HIGH);
      digitalWrite(L_L_EN, HIGH);
      digitalWrite(R_R_EN, HIGH);
      digitalWrite(R_L_EN, HIGH);

      Serial.println(F("STATUS IDLE"));
      Serial.println(F(">> Probe fully retracted (7s elapsed). Sensor set to "
                       "IDLE. Sequence complete."));
      Serial.println(F(">> Remote control unlocked. Ready to navigate."));
      Serial.println(
          F("----------------------------------------------------------\n"));
      Serial.flush();

      actState = ACT_IDLE;
    }
    break;

  case ACT_IDLE:
  default:
    break;
  }
}

void stopActuatorRelay() {
  digitalWrite(RELAY_ACT_1, HIGH);
  digitalWrite(RELAY_ACT_2, HIGH);
}

// =========================================================================
// UNIVERSAL 7-IN-1 SENSOR RS485 READ ROUTINE (DUAL PROTOCOL AUTO-DETECT)
// =========================================================================

void readNPKSensor() {
  float moisture = 0.0, temperature = 0.0, ph = 0.0;
  uint16_t ec = 0;
  uint16_t n_0x00 = 0, p_0x00 = 0, k_0x00 = 0;
  uint16_t n_0x1e = 0, p_0x1e = 0, k_0x1e = 0;

  // -------------------------------------------------------------
  // QUERY 1: Read 7 registers starting at 0x0000 (Checks standard map)
  // -------------------------------------------------------------
  while (npkSerial.available()) {
    npkSerial.read();
  }

  digitalWrite(RS485_DE_RE, HIGH); // Transmit mode
  delay(10);
  npkSerial.write(allParamsRequestFrame, sizeof(allParamsRequestFrame));
  npkSerial.flush();
  delay(15); // Wait for transmission of last bit on 4800 baud before disabling driver
  digitalWrite(RS485_DE_RE, LOW); // Receive mode
  delay(10);

  unsigned long timeout = millis();
  byte idx1 = 0;
  while ((millis() - timeout < 600) &&
         (idx1 < sizeof(allParamsResponseFrame))) {
    if (npkSerial.available()) {
      allParamsResponseFrame[idx1++] = npkSerial.read();
    }
  }

  bool q1_ok = false;
  if (idx1 >= 19) {
    q1_ok = true;
    moisture =
        ((allParamsResponseFrame[3] << 8) | allParamsResponseFrame[4]) / 10.0;
    temperature =
        ((allParamsResponseFrame[5] << 8) | allParamsResponseFrame[6]) / 10.0;
    ec = (allParamsResponseFrame[7] << 8) | allParamsResponseFrame[8];
    ph = ((allParamsResponseFrame[9] << 8) | allParamsResponseFrame[10]) / 10.0;
    n_0x00 = (allParamsResponseFrame[11] << 8) | allParamsResponseFrame[12];
    p_0x00 = (allParamsResponseFrame[13] << 8) | allParamsResponseFrame[14];
    k_0x00 = (allParamsResponseFrame[15] << 8) | allParamsResponseFrame[16];
  }

  // Generous turnaround delay to allow RS485 bus and sensor to settle
  delay(150);

  // -------------------------------------------------------------
  // QUERY 2: Read 3 registers starting at 0x001E (Checks JXCT/CWT map)
  // -------------------------------------------------------------
  while (npkSerial.available()) {
    npkSerial.read();
  }

  digitalWrite(RS485_DE_RE, HIGH); // Transmit mode
  delay(10);
  npkSerial.write(npkRequestFrame, sizeof(npkRequestFrame));
  npkSerial.flush();
  delay(15);
  digitalWrite(RS485_DE_RE, LOW); // Receive mode
  delay(10);

  timeout = millis();
  byte idx2 = 0;
  while ((millis() - timeout < 600) &&
         (idx2 < sizeof(npkResponseFrame))) {
    if (npkSerial.available()) {
      npkResponseFrame[idx2++] = npkSerial.read();
    }
  }

  bool q2_ok = false;
  if (idx2 >= 11) {
    q2_ok = true;
    n_0x1e = (npkResponseFrame[3] << 8) | npkResponseFrame[4];
    p_0x1e = (npkResponseFrame[5] << 8) | npkResponseFrame[6];
    k_0x1e = (npkResponseFrame[7] << 8) | npkResponseFrame[8];
  }

  // Auto-detect which register set returned actual values
  uint16_t final_n = (n_0x1e > 0) ? n_0x1e : n_0x00;
  uint16_t final_p = (p_0x1e > 0) ? p_0x1e : p_0x00;
  uint16_t final_k = (k_0x1e > 0) ? k_0x1e : k_0x00;

  // Debug line to see if sensor actually answered
  Serial.print(F("[SENSOR RESP]: Frame1="));
  Serial.print(idx1);
  Serial.print(F("/19B  Frame2="));
  Serial.print(idx2);
  Serial.println(F("/11B"));

  // Output formatted fields to Raspberry Pi (matches serial_comm.py)
  Serial.println(F("--- 7-IN-1 SOIL SENSOR READINGS ---"));
  Serial.print(F("Moisture    : "));
  Serial.print(moisture);
  Serial.println(F(" %"));
  Serial.print(F("Temperature : "));
  Serial.print(temperature);
  Serial.println(F(" deg C"));
  Serial.print(F("EC          : "));
  Serial.print(ec);
  Serial.println(F(" us/cm"));
  Serial.print(F("pH Level    : "));
  Serial.println(ph);
  Serial.print(F("Nitrogen (N): "));
  Serial.print(final_n);
  Serial.println(F(" mg/kg"));
  Serial.print(F("Phosphorus  : "));
  Serial.print(final_p);
  Serial.println(F(" mg/kg"));
  Serial.print(F("Potassium   : "));
  Serial.print(final_k);
  Serial.println(F(" mg/kg"));
  Serial.println(F("-----------------------------------"));
  Serial.flush();
}

// =========================================================================
// DRIVETRAIN MOTOR CONTROL ROUTINES (BTS7960)
// =========================================================================

void motorForward() {
  analogWrite(L_RPWM, 0);
  analogWrite(R_RPWM, 0);
  analogWrite(L_LPWM, MOTOR_SPEED);
  analogWrite(R_LPWM, MOTOR_SPEED);
}

void motorBackward() {
  analogWrite(L_LPWM, 0);
  analogWrite(R_LPWM, 0);
  analogWrite(L_RPWM, MOTOR_SPEED);
  analogWrite(R_RPWM, MOTOR_SPEED);
}

void motorLeft() {
  analogWrite(L_LPWM, 0);
  analogWrite(R_RPWM, 0);
  analogWrite(L_RPWM, MOTOR_SPEED);
  analogWrite(R_LPWM, MOTOR_SPEED);
}

void motorRight() {
  analogWrite(R_LPWM, 0);
  analogWrite(L_RPWM, 0);
  analogWrite(L_LPWM, MOTOR_SPEED);
  analogWrite(R_RPWM, MOTOR_SPEED);
}

void motorStop() {
  analogWrite(L_RPWM, 0);
  analogWrite(L_LPWM, 0);
  analogWrite(R_RPWM, 0);
  analogWrite(R_LPWM, 0);
}