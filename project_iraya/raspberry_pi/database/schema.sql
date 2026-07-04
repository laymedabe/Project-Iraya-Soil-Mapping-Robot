-- Project Iraya — MariaDB schema
-- Run with: mysql -u root -p < schema.sql

CREATE DATABASE IF NOT EXISTS project_iraya
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE USER IF NOT EXISTS 'iraya'@'localhost' IDENTIFIED BY 'changeme';
GRANT ALL PRIVILEGES ON project_iraya.* TO 'iraya'@'localhost';
FLUSH PRIVILEGES;

USE project_iraya;

-- One row per field run (from "Start Run" to completion/abort)
CREATE TABLE IF NOT EXISTS sessions (
    id INT AUTO_INCREMENT PRIMARY KEY,
    field_name VARCHAR(120) NOT NULL DEFAULT 'Unnamed Field',
    started_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    ended_at DATETIME NULL,
    status ENUM('running', 'paused', 'completed', 'aborted') NOT NULL DEFAULT 'running',
    lat_min DECIMAL(9,6), lat_max DECIMAL(9,6),
    lon_min DECIMAL(9,6), lon_max DECIMAL(9,6),
    notes TEXT NULL,
    synced_to_cloud TINYINT(1) NOT NULL DEFAULT 0
) ENGINE=InnoDB;

-- Planned sampling waypoints for a session (generated at session start)
CREATE TABLE IF NOT EXISTS waypoints (
    id INT AUTO_INCREMENT PRIMARY KEY,
    session_id INT NOT NULL,
    seq_index INT NOT NULL,           -- order in the path
    lat DECIMAL(9,6) NOT NULL,
    lon DECIMAL(9,6) NOT NULL,
    visited TINYINT(1) NOT NULL DEFAULT 0,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    INDEX idx_session_seq (session_id, seq_index)
) ENGINE=InnoDB;

-- Actual sensor readings collected at each waypoint
CREATE TABLE IF NOT EXISTS readings (
    id INT AUTO_INCREMENT PRIMARY KEY,
    session_id INT NOT NULL,
    waypoint_id INT NULL,
    recorded_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    lat DECIMAL(9,6) NOT NULL,
    lon DECIMAL(9,6) NOT NULL,
    nitrogen DECIMAL(6,2) NOT NULL,     -- mg/kg
    phosphorus DECIMAL(6,2) NOT NULL,   -- mg/kg
    potassium DECIMAL(6,2) NOT NULL,    -- mg/kg
    moisture DECIMAL(5,2) NULL,         -- %
    temperature DECIMAL(5,2) NULL,      -- deg C
    ec DECIMAL(5,3) NULL,               -- dS/m
    battery_pct DECIMAL(5,2) NULL,
    synced_to_cloud TINYINT(1) NOT NULL DEFAULT 0,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (waypoint_id) REFERENCES waypoints(id) ON DELETE SET NULL,
    INDEX idx_session (session_id),
    INDEX idx_sync (synced_to_cloud)
) ENGINE=InnoDB;

-- Fault/event log from the Mega (watchdog trips, E-STOP, actuator stalls)
CREATE TABLE IF NOT EXISTS system_events (
    id INT AUTO_INCREMENT PRIMARY KEY,
    session_id INT NULL,
    occurred_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    level ENUM('info', 'warning', 'fault') NOT NULL DEFAULT 'info',
    source VARCHAR(40) NOT NULL,        -- e.g. 'mega', 'flask', 'socketio'
    message VARCHAR(255) NOT NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE SET NULL
) ENGINE=InnoDB;
