#include "npk_sensor.h"
#include "config.h"

/*
 * Modbus RTU driver for a typical 7-in-1 RS485 soil sensor
 * (moisture / temperature / EC / pH / N / P / K).
 *
 * IMPORTANT: register addresses and scaling below match the common
 * "JXCT / RS-WS-N01-TR"-style register map used by many inexpensive
 * RS485 NPK probes, but register maps vary between manufacturers.
 * Confirm against your specific sensor's datasheet before trusting
 * these values — if readings look implausible (e.g. off by 10x), it's
 * almost always a scaling-factor mismatch here.
 *
 * Register layout assumed (holding registers, function code 0x03):
 *   0x0000  Moisture   (raw / 10 = %)
 *   0x0001  Temperature(raw / 10 = deg C)
 *   0x0002  EC         (raw = us/cm; divide by 1000 for dS/m)
 *   0x0003  pH         (raw / 10, unused here but read to keep register block contiguous)
 *   0x0004  Nitrogen   (raw = mg/kg)
 *   0x0005  Phosphorus (raw = mg/kg)
 *   0x0006  Potassium  (raw = mg/kg)
 */

#define NPK_REG_START   0x0000
#define NPK_REG_COUNT   7

static uint16_t modbusCRC16(uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (int i = 0; i < 8; i++) {
      if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
      else { crc >>= 1; }
    }
  }
  return crc;
}

void npkInit() {
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW); // start in receive mode
  RS485_SERIAL.begin(4800);           // most RS485 NPK sensors default to 4800 8N1 — confirm on your unit
}

static void rs485Transmit(uint8_t *frame, int len) {
  digitalWrite(RS485_DE_RE_PIN, HIGH);
  delayMicroseconds(50);
  RS485_SERIAL.write(frame, len);
  RS485_SERIAL.flush();
  delayMicroseconds(50);
  digitalWrite(RS485_DE_RE_PIN, LOW);
}

NpkReading npkRead() {
  NpkReading result = {0, 0, 0, 0, 0, 0, false};

  uint8_t request[8];
  request[0] = NPK_SLAVE_ID;
  request[1] = 0x03; // Read Holding Registers
  request[2] = (NPK_REG_START >> 8) & 0xFF;
  request[3] = NPK_REG_START & 0xFF;
  request[4] = (NPK_REG_COUNT >> 8) & 0xFF;
  request[5] = NPK_REG_COUNT & 0xFF;
  uint16_t crc = modbusCRC16(request, 6);
  request[6] = crc & 0xFF;
  request[7] = (crc >> 8) & 0xFF;

  while (RS485_SERIAL.available()) RS485_SERIAL.read(); // flush stale bytes
  rs485Transmit(request, 8);

  // Expected response: [slaveID][0x03][byteCount][data...][crcLo][crcHi]
  int expectedLen = 3 + (NPK_REG_COUNT * 2) + 2;
  uint8_t response[64];
  int received = 0;
  unsigned long start = millis();

  while (received < expectedLen && millis() - start < 200) {
    if (RS485_SERIAL.available()) {
      response[received++] = RS485_SERIAL.read();
    }
  }

  if (received < expectedLen) {
    return result; // timeout — sensor didn't respond in time
  }

  uint16_t receivedCrc = response[expectedLen - 2] | (response[expectedLen - 1] << 8);
  uint16_t calcCrc = modbusCRC16(response, expectedLen - 2);
  if (receivedCrc != calcCrc) {
    return result; // CRC mismatch — corrupted frame, discard
  }

  auto regAt = [&](int i) -> uint16_t {
    int offset = 3 + (i * 2);
    return (response[offset] << 8) | response[offset + 1];
  };

  result.moisture    = regAt(0) / 10.0f;
  result.temperature = regAt(1) / 10.0f;
  result.ec          = regAt(2) / 1000.0f;
  // regAt(3) is pH, unused for now but kept in the block above
  result.nitrogen    = (float)regAt(4);
  result.phosphorus  = (float)regAt(5);
  result.potassium   = (float)regAt(6);
  result.valid = true;

  return result;
}
