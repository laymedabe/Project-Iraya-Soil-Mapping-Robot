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

// Right Motor Driver (BTS7960 #2) - Timer 1 PWM (Avoids Timer 2 conflicts with IRremote)
const int R_RPWM = 9; 
const int R_LPWM = 10; 
const int R_R_EN = 4; 
const int R_L_EN = 12; 

// Linear Actuator 2-Channel Power Relay Module
const int RELAY_ACT_1 = A0; // Controls Actuator Lead 1
const int RELAY_ACT_2 = A1; // Controls Actuator Lead 2

// MAX485 TTL RS485 Module Pins for 7-in-1 Sensor
const int RS485_RX = A2; // Arduino RX <- MAX485 RO
const int RS485_TX = A3; // Arduino TX -> MAX485 DI
const int RS485_DE_RE = A4; // RS485 Flow Control (DE and RE tied together)

SoftwareSerial npkSerial(RS485_RX, RS485_TX);

// --- MODBUS RTU COMMAND FRAMES ---
// Query 1: 4 registers starting at 0x0000 -> Moisture, Temperature, EC, pH
const byte envRequestFrame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x04, 0x44, 0x09};
byte envResponseFrame[13]; // 3 header + 8 data + 2 CRC = 13 bytes

// Query 2: 3 registers starting at 0x001E -> Nitrogen, Phosphorus, Potassium
const byte npkRequestFrame[] = {0x01, 0x03, 0x00, 0x1E, 0x00, 0x03, 0x65, 0xCD};
byte npkResponseFrame[11]; // 3 header + 6 data + 2 CRC = 11 bytes

// =========================================================================
// SPEED & TIMING PARAMETERS
// =========================================================================

const int MOTOR_SPEED        = 200;   // Drivetrain PWM Speed (0 to 255)
const unsigned long EXTEND_TIME  = 7000;  // Actuator extend duration (ms)
const unsigned long HOLD_TIME    = 10000; // Time probe stays lowered (ms)
const unsigned long RETRACT_TIME = 7000;  // Actuator retract duration (ms)
const unsigned long SENSOR_INTERVAL = 2000; // Read sensor every 2 seconds during hold

// =========================================================================
// IR REMOTE HEX COMMANDS
// =========================================================================

const unsigned long IR_FORWARD  = 0xBC43FF00; 
const unsigned long IR_BACKWARD = 0xBB44FF00; 
const unsigned long IR_LEFT     = 0xB946FF00; 
const unsigned long IR_RIGHT    = 0xEA15FF00; 
const unsigned long IR_STOP     = 0xAD52FF00; 
const unsigned long IR_ACTUATE  = 0xBF40FF00; // Triggers Linear Actuator Sequence

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
  npkSerial.begin(4800); // 7-in-1 Sensor default baud rate

  // MAX485 Control Pin Setup
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW); // Receive mode by default

  // Safely set relays off BEFORE declaring outputs to prevent startup relay clicks
  digitalWrite(RELAY_ACT_1, HIGH);
  digitalWrite(RELAY_ACT_2, HIGH);
  pinMode(RELAY_ACT_1, OUTPUT);
  pinMode(RELAY_ACT_2, OUTPUT);

  // Motor Driver Pin Modes
  pinMode(L_RPWM, OUTPUT); pinMode(L_LPWM, OUTPUT);
  pinMode(L_R_EN, OUTPUT); pinMode(L_L_EN, OUTPUT);
  pinMode(R_RPWM, OUTPUT); pinMode(R_LPWM, OUTPUT);
  pinMode(R_R_EN, OUTPUT); pinMode(R_L_EN, OUTPUT);

  // Enable BTS7960 Drivers
  digitalWrite(L_R_EN, HIGH); digitalWrite(L_L_EN, HIGH);
  digitalWrite(R_R_EN, HIGH); digitalWrite(R_L_EN, HIGH);

  // Start IR Receiver
  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  motorStop();

  Serial.println(F("=========================================================="));
  Serial.println(F("     ARDUINO UNO ROBOT ACTUATOR SYSTEM INITIALIZED        "));
  Serial.println(F("=========================================================="));
  Serial.flush();
}

// =========================================================================
// MAIN CONTROL LOOP
// =========================================================================

void loop() {
  // Check IR remote ONLY if the actuator cycle is NOT active (24-second lockout)
  if (actState == ACT_IDLE) {
    if (IrReceiver.decode()) {
      unsigned long receivedCode = IrReceiver.decodedIRData.decodedRawData;

      if (receivedCode != 0 && !(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
        if (receivedCode == IR_FORWARD)       { motorForward(); } 
        else if (receivedCode == IR_BACKWARD) { motorBackward(); } 
        else if (receivedCode == IR_LEFT)     { motorLeft(); }
        else if (receivedCode == IR_RIGHT)    { motorRight(); }
        else if (receivedCode == IR_STOP)     { motorStop(); stopActuatorRelay(); }
        else if (receivedCode == IR_ACTUATE)  {
          motorStop(); // Lock wheels
          
          // Anti-brownout / anti-glitch protection:
          // Disable drivetrain drivers to cut idle electrical noise
          digitalWrite(L_R_EN, LOW); digitalWrite(L_L_EN, LOW);
          digitalWrite(R_R_EN, LOW); digitalWrite(R_L_EN, LOW);

          // Flush serial buffer BEFORE energizing relays to prevent USB packet corruption
          Serial.println(F("\n>> ACTUATOR STARTED: All remote buttons locked for 24s..."));
          Serial.flush();
          delay(80); // Let power rail stabilize before relay switch

          startActuatorExtend();
        }
      }

      IrReceiver.resume(); 
    }

    // Also support web dashboard commands
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd == "SAMPLE" || cmd == "ACTUATE") {
        motorStop();
        digitalWrite(L_R_EN, LOW); digitalWrite(L_L_EN, LOW);
        digitalWrite(R_R_EN, LOW); digitalWrite(R_L_EN, LOW);

        Serial.println(F("\n>> ACTUATOR STARTED: Command received..."));
        Serial.flush();
        delay(80);

        startActuatorExtend();
      }
    }
  } else {
    // Clear out any buffered IR signals received during the 24s lockout period
    if (IrReceiver.decode()) {
      IrReceiver.resume();
    }
  }

  // Non-blocking state machine for linear actuator
  updateActuatorSequence();
}

// =========================================================================
// ACTUATOR AUTOMATION & STATE MACHINE
// =========================================================================

void startActuatorExtend() {
  actState = ACT_EXTENDING;
  actTimer = millis();
  
  // Extend Actuator (Active-LOW Relays)
  digitalWrite(RELAY_ACT_1, LOW);  // Lead 1 -> +12V
  digitalWrite(RELAY_ACT_2, HIGH); // Lead 2 -> GND
}

void updateActuatorSequence() {
  switch (actState) {
    
    case ACT_EXTENDING:
      if (millis() - actTimer >= EXTEND_TIME) {
        stopActuatorRelay();
        delay(50); // Dead-time settling delay

        Serial.println(F(">> Probe fully lowered (7s elapsed). Starting 10s continuous monitoring..."));
        Serial.flush();

        actState = ACT_HOLDING;
        actTimer = millis();
        lastSensorReadTimer = 0; // Trigger immediate first reading
      }
      break;

    case ACT_HOLDING:
      // Read sensor every SENSOR_INTERVAL ms throughout the hold period
      if (millis() - lastSensorReadTimer >= SENSOR_INTERVAL) {
        lastSensorReadTimer = millis();
        readNPKSensor();
      }

      if (millis() - actTimer >= HOLD_TIME) {
        Serial.println(F(">> Hold duration complete (10s elapsed). Retracting for 7s..."));
        Serial.flush();
        delay(50); // Dead-time settling delay

        actState = ACT_RETRACTING;
        actTimer = millis();
        
        // Retract Actuator (Active-LOW Relays)
        digitalWrite(RELAY_ACT_1, HIGH); // Lead 1 -> GND
        digitalWrite(RELAY_ACT_2, LOW);  // Lead 2 -> +12V
      }
      break;

    case ACT_RETRACTING:
      if (millis() - actTimer >= RETRACT_TIME) {
        stopActuatorRelay();
        delay(50);

        // Re-enable drivetrain BTS7960 motor drivers
        digitalWrite(L_R_EN, HIGH); digitalWrite(L_L_EN, HIGH);
        digitalWrite(R_R_EN, HIGH); digitalWrite(R_L_EN, HIGH);

        Serial.println(F(">> Probe fully retracted (7s elapsed). Sensor set to IDLE. Sequence complete."));
        Serial.println(F(">> Remote control unlocked. Ready to navigate."));
        Serial.println(F("----------------------------------------------------------\n"));
        Serial.flush();

        actState = ACT_IDLE; // Unlocks remote buttons
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

// =========================================================================
// 7-IN-1 SENSOR RS485 READ ROUTINE (DUAL REGISTER QUERY)
// =========================================================================

void readNPKSensor() {
  float moisture = 0.0, temperature = 0.0, ph = 0.0;
  uint16_t ec = 0, nitrogen = 0, phos = 0, potass = 0;

  // -------------------------------------------------------------
  // QUERY 1: Moisture, Temperature, EC, pH (Starting at Address 0x0000)
  // -------------------------------------------------------------
  while (npkSerial.available()) { npkSerial.read(); }

  digitalWrite(RS485_DE_RE, HIGH); // Transmit mode
  delay(10);
  npkSerial.write(envRequestFrame, sizeof(envRequestFrame));
  npkSerial.flush();
  digitalWrite(RS485_DE_RE, LOW);  // Receive mode
  delay(10);

  unsigned long timeout = millis();
  byte index = 0;
  while ((millis() - timeout < 500) && (index < sizeof(envResponseFrame))) {
    if (npkSerial.available()) {
      envResponseFrame[index++] = npkSerial.read();
    }
  }

  if (index >= 13) {
    moisture    = ((envResponseFrame[3] << 8) | envResponseFrame[4]) / 10.0;
    temperature = ((envResponseFrame[5] << 8) | envResponseFrame[6]) / 10.0;
    ec          =  (envResponseFrame[7] << 8) | envResponseFrame[8];
    ph          = ((envResponseFrame[9] << 8) | envResponseFrame[10]) / 10.0;
  }

  delay(60); // Essential turnaround delay between consecutive Modbus frames

  // -------------------------------------------------------------
  // QUERY 2: Nitrogen, Phosphorus, Potassium (Starting at Address 0x001E)
  // -------------------------------------------------------------
  while (npkSerial.available()) { npkSerial.read(); }

  digitalWrite(RS485_DE_RE, HIGH); // Transmit mode
  delay(10);
  npkSerial.write(npkRequestFrame, sizeof(npkRequestFrame));
  npkSerial.flush();
  digitalWrite(RS485_DE_RE, LOW);  // Receive mode
  delay(10);

  timeout = millis();
  index = 0;
  while ((millis() - timeout < 500) && (index < sizeof(npkResponseFrame))) {
    if (npkSerial.available()) {
      npkResponseFrame[index++] = npkSerial.read();
    }
  }

  if (index >= 11) {
    nitrogen = (npkResponseFrame[3] << 8) | npkResponseFrame[4];
    phos     = (npkResponseFrame[5] << 8) | npkResponseFrame[6];
    potass   = (npkResponseFrame[7] << 8) | npkResponseFrame[8];
  }

  // -------------------------------------------------------------
  // OUTPUT TO RASPBERRY PI OVER USB SERIAL
  // -------------------------------------------------------------
  Serial.println(F("--- 7-IN-1 SOIL SENSOR READINGS ---"));
  Serial.print(F("Moisture    : ")); Serial.print(moisture); Serial.println(F(" %"));
  Serial.print(F("Temperature : ")); Serial.print(temperature); Serial.println(F(" deg C"));
  Serial.print(F("EC          : ")); Serial.print(ec); Serial.println(F(" us/cm"));
  Serial.print(F("pH Level    : ")); Serial.println(ph);
  Serial.print(F("Nitrogen (N): ")); Serial.print(nitrogen); Serial.println(F(" mg/kg"));
  Serial.print(F("Phosphorus  : ")); Serial.print(phos); Serial.println(F(" mg/kg"));
  Serial.print(F("Potassium   : ")); Serial.print(potass); Serial.println(F(" mg/kg"));
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