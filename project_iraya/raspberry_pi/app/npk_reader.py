import serial
import threading
import time
import random
import logging
from app.config import Config

logger = logging.getLogger(__name__)

class NPKReader:
    def __init__(self):
        self.port = Config.NPK_PORT
        self.baud = Config.NPK_BAUD
        self.simulate = Config.NPK_SIMULATE
        self.ser = None
        self._lock = threading.Lock()
        
        # Modbus RTU query frame for JXCT NPK sensor (matches Mega code)
        self.query_frame = bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B])
        
    def start(self):
        if not self.simulate:
            try:
                self.ser = serial.Serial(self.port, self.baud, timeout=1.0)
                logger.info(f"Opened NPK port: {self.port} at {self.baud} baud")
            except Exception as e:
                logger.error(f"Failed to open NPK port: {e}. Falling back to simulation.")
                self.simulate = True

    def stop(self):
        if self.ser:
            self.ser.close()

    def read_npk(self):
        """Reads NPK values from the sensor. Returns dict with N, P (and K if available), or None if invalid."""
        if self.simulate:
            return {
                'nitrogen': random.uniform(10.0, 30.0),
                'phosphorus': random.uniform(5.0, 20.0),
                'potassium': random.uniform(40.0, 100.0)
            }
            
        with self._lock:
            try:
                # Flush buffers
                self.ser.reset_input_buffer()
                self.ser.reset_output_buffer()
                
                # Send query
                self.ser.write(self.query_frame)
                
                # The response should be 9 bytes:
                # [0x01] [0x03] [0x04] [N_Hi] [N_Lo] [P_Hi] [P_Lo] [CRC_Lo] [CRC_Hi]
                response = self.ser.read(9)
                
                if len(response) == 9 and response[0] == 0x01 and response[1] == 0x03 and response[2] == 0x04:
                    n = (response[3] << 8) | response[4]
                    p = (response[5] << 8) | response[6]
                    return {
                        'nitrogen': float(n),
                        'phosphorus': float(p),
                        'potassium': 0.0  # Sensor only returns N and P based on this query
                    }
                else:
                    logger.warning(f"Invalid NPK response length or header: {response.hex()}")
                    return None
            except serial.SerialException as e:
                logger.error(f"NPK Serial error: {e}")
                return None
            except Exception as e:
                logger.error(f"NPK unexpected error: {e}")
                return None

npk_reader = NPKReader()
