## ICCP — Intelligent Incubator Communication Protocol

Intelligent Incubator Communication Protocol, is a draft protocol created for a college class at the University of São Paulo (USP)

### Architecture

| Component | File | Role |
|---|---|---|
| **Protocol library** | `src/protocol/protocol.{hpp,cpp}` | Binary message framing (TCP send/receive, `create_message`) |
| **Common types** | `src/common/common.hpp` | `device_type`, `connect`, `disconnect` structs |
| **Manager (server)** | `src/manager/manager.{hpp,cpp}` | Central server — accepts connections, runs control loop |
| **Sensor** | `src/sensor/sensor.{hpp,cpp}` | Simulated sensor sending periodic `SENSOR_DATA` |
| **Actuator** | `src/actuator/actuator.{hpp,cpp}` | Listens for `ACTUATOR_CMD`, responds with `ACTUATOR_ACK` |
| **Client** | `src/client/client.{hpp,cpp}` | Sends `READ_REQUEST` / `CONFIG`, receives `READ_RESPONSE` / `ALARM` |

### Implemented Features

- **Connection flow**: Sensors and actuators send `CONNECT` → Manager responds with `CONNECT_ACK` (status + interval)
- **Sensor data**: Sensors send `SENSOR_DATA` every 1000ms (configurable via CONFIG)
- **Hysteresis control**: When `value > max + hysteresis` → actuator ON; when `value ≤ max` → OFF. Temperature sensor mapped to FAN actuator.
- **Alarms**: O2 sensor (type `0x03`) and BPM sensor (type `0x04`) trigger `ALARM` when `value < min`. Alarms re-sent every poll cycle while condition persists, and clear automatically when value recovers.
- **READ_REQUEST / READ_RESPONSE**: Client queries individual sensors or all (`0xFF`), gets last stored readings.
- **CONFIG / CONFIG_ACK**: Client adjusts min/max/hysteresis per device type. Validated by manager.
- **Multi-connection**: Manager uses `poll()` to handle concurrent sensor, actuator, and client connections.

### Build & Run

```sh
cmake -S . -B build && cmake --build build

# Start manager (default port 9000)
./build/iccp-manager [port]

# Start sensor
./build/iccp-sensor <device_id> <temperature|umidity|oxygen|bpm> <server_ip> [port] [base_value]

# Start actuator
./build/iccp-actuator <device_id> <heater|humidifier|fan> <server_ip> [port]

# Start client (interactive)
./build/iccp-client <client_id> <server_ip> [port]
#   Commands: read [sensor_id|all], config <type> <min> <max> <hyst>
```
