# Getting Started with Project Iraya

This guide provides step-by-step instructions on how to set up the hardware, flash the Arduino Mega, configure the Raspberry Pi, and start the web application.

## 1. Hardware Connection

The Raspberry Pi and the Arduino Mega communicate over a UART serial connection via USB.

1. Connect the Arduino Mega to one of the USB ports on the Raspberry Pi 5 using a standard USB A-to-B cable.
2. The serial port on the Raspberry Pi will typically be identified as `/dev/ttyACM0` or `/dev/ttyUSB0`. You can verify this by running `ls /dev/tty*` in the Raspberry Pi terminal before and after connecting the cable.

## 2. Flashing the Arduino Mega Firmware

The Arduino Mega is responsible for real-time motor control, safety watchdogs, and hardware I/O.

1. Disconnect the Arduino Mega from the Raspberry Pi and connect it to your development computer.
2. Open the Arduino IDE or PlatformIO.
3. Open the firmware sketch located at:
   `arduino_mega/project_iraya_mega/project_iraya_mega.ino`
4. Review the pin assignments in `arduino_mega/project_iraya_mega/config.h` and update them if necessary to match your physical wiring.
5. Install any required libraries mentioned at the top of the `.ino` file.
6. Select the correct COM port and board (Arduino Mega 2560), then **Compile and Upload** the firmware.
7. Once successfully flashed, reconnect the Arduino Mega to the Raspberry Pi.

## 3. Raspberry Pi Environment Setup

The Raspberry Pi runs the main Python Flask application and provides the web dashboard. Ensure your Raspberry Pi is running Raspberry Pi OS Bookworm (64-bit).

1. **Open a terminal** on the Raspberry Pi and navigate to the project directory:
   ```bash
   cd project_iraya/raspberry_pi
   ```

2. **Set up a Python virtual environment:**
   ```bash
   python3 -m venv venv
   source venv/bin/activate
   ```

3. **Install the required dependencies:**
   ```bash
   pip install -r requirements.txt
   ```

4. **Configure Environment Variables:**
   - Copy the sample `.env` file to create your local configuration:
     ```bash
     cp .env.example .env
     ```
     *(Note: If an example file doesn't exist, create a `.env` file based on the environment variables needed in `app/config.py`)*
   - Open `.env` and verify the `IRAYA_SERIAL_PORT` matches your hardware connection (e.g., `/dev/ttyACM0`).
   - If you are running the app without an Arduino connected (for development), set `IRAYA_SERIAL_SIMULATE=true`. Make sure this is `false` when running on the actual robot.

## 4. Database Initialization

The app uses MariaDB for storing sensor readings and map data.

1. Ensure MariaDB is installed and running on the Raspberry Pi.
2. Load the initial schema:
   ```bash
   mysql -u root -p < database/schema.sql
   ```
3. Run the setup script to verify the connection and seed a demo session:
   ```bash
   python scripts/init_db.py
   ```

## 5. Starting the Application

With the environment set up and the database initialized, you can now start the web application.

1. Make sure you are in the `raspberry_pi` directory with the virtual environment activated.
2. Run the entry point script:
   ```bash
   python run.py
   ```

## 6. Accessing the Dashboard

The dashboard allows you to view sensor data and manually drive the robot.

1. Connect your laptop, tablet, or smartphone to the Raspberry Pi's Wi-Fi hotspot.
2. Open a web browser and navigate to the Pi's IP address on port 5000. For a default hotspot configuration, this is usually:
   `http://192.168.4.1:5000`
3. You should now see the Project Iraya web dashboard and can begin interacting with the robot!
