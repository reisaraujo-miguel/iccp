#pragma once

#include <cstdint>

enum class connect_stat : uint8_t {
  ACCEPTED = 0x00,
  REJECT_DUP_ID = 0x01,
  REJECT_UNSUPPORTED_TYPE = 0x02,
};

enum class config_stat : uint8_t {
  ACCEPTED = 0x00,
  REJECTED = 0x01,
};

enum class read_resp_stat : uint8_t {
  OK = 0x00,
  SENSOR_NOT_FOUND = 0x01,
  NO_DATA = 0x02,
};

enum class alarm_type : uint8_t {
  CRITICAL_OXYGENATION = 0x01,
  CRITICAL_BPM = 0x02,
  TEMPERATURE_OUT_OF_RANGE = 0x03,
  UMIDITY_OUT_OF_RANGE = 0x04,
};

enum class alarm_severity : uint8_t {
  WARNING = 0x00,
  CRITICAL = 0x01,
  EMERGENCY = 0x02,
};

#pragma pack(push, 1)

// connect_ack message sent from the manager to a device in response to a
// connect message. It indicates whether the connection was accepted or
// rejected.
//
// - sequence_ref:  the sequence number of the CONNECT message being
// acknowledged
//
// - status:        the status of the connection attempt (accepted or rejected)
//
// - interval_ms:   the recommended reporting interval in milliseconds (default:
// 1000)
//
// - reason[32]:    optional rejection reason, right padded with "\0" to 32
// bytes
struct connect_ack {
  uint16_t sequence_ref;
  connect_stat status;
  uint16_t interval_ms;
  char reason[32];
};

// read_response message sent from the manager to a device in response to a
// read_request message. It contains the requested sensor readings or an error
// status.
//
// - sequence_ref:  the sequence number of the READ_REQUEST message being
// acknowledged
// - status:        the status of the read request (OK, sensor not found, or no
// data available)
// - num_entries:   how many sensor readings are included in the response (1 -
// 255)
// - sensor readings follow in the payload, each 26 bytes long:
//   - device_id[16] (right padded with "\0")
//   - d_type[1] (device_type enum)
//   - value[4] (float)
//   - timestamp[4] (right padded with "\0")
//   - status[1] (sensor_status enum)
struct read_response {
  uint16_t sequence_ref;
  read_resp_stat status;
  uint8_t num_entries;
};

// alarm message sent from the manager to the client to notify it of an alarm
// condition. It contains the details of the alarm event.
//
// - alarm_id:      unique identifier for the alarm event
// - a_type:        the type of alarm (alarm_type enum)
// - severity:      the severity of the alarm (alarm_severity enum)
// - device_id[16]: ASCII identification of the device that triggered the alarm,
//                  must be right padded with "\0" to 16 bytes
// - value:         the sensor reading that triggered the alarm (float)
// - threshold:     the threshold value that was exceeded (float)
// - timestamp:     the timestamp of the alarm event (uint32_t)
// - message[64]:   optional alarm message, right padded with "\0" to 64 bytes
struct alarm {
  uint32_t alarm_id;
  alarm_type a_type;
  alarm_severity severity;
  char device_id[16];
  float value;
  float threshold;
  uint32_t timestamp;
  char message[64];
};

// config_ack message sent from the manager to a device in response to a
// config message. It indicates whether the configuration was accepted or
// rejected.
//
// - sequence_ref:  the sequence number of the CONFIG message being acknowledged
// - status:        the status of the configuration attempt (accepted or
// rejected)
// - num_errors:    how many configuration errors are included in the response
//                  (0 - 255)
// - configuration errors follow in the payload, each 2 bytes long:
//   - index[1] (index of the parameter in the original CONFIG message)
//   - error_code[1] (error code, 0x00 = no error, 0x01 = invalid value, 0x02 =
//   unsupported parameter)
struct config_ack {
  uint16_t sequence_ref;
  config_stat status;
  uint8_t num_errors;
};

#pragma pack(pop) // Restore original compiler alignment settings

// Manager class and functions

#include "../actuator/actuator.hpp"
#include "../common/common.hpp"
#include "../sensor/sensor.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct unpacked_message;

// Per-device-type configuration limits
struct Limits {
  float min = 0;
  float max = 100;
  float hysteresis = 1.0f;
};

class Manager {
public:
  explicit Manager(uint16_t port = 9000);
  ~Manager();

  void run();

private:
  // device tracking
  struct DeviceInfo {
    int fd = -1;
    std::string id; // 16 bytes, null-padded
    device_type type{};
  };

  struct Reading {
    float value = 0;
    uint32_t timestamp = 0;
    sensor_stat status = sensor_stat::OK;
    std::string unit;
    device_type type{};
  };

  struct ActiveAlarm {
    uint32_t alarm_id = 0;
    alarm_type a_type{};
    alarm_severity severity{};
    std::string device_id;
    float value = 0;
    float threshold = 0;
    uint32_t timestamp = 0;
  };

  // IO
  int listen_fd_ = -1;
  uint16_t port_;
  uint16_t seq_;
  uint32_t next_alarm_id_ = 1;

  // state
  std::unordered_map<int, DeviceInfo> devices_; // fd → device
  std::unordered_map<std::string, Reading>
      last_readings_; // sensor_id → reading
  std::unordered_map<std::string, bool>
      actuator_states_;                             // actuator_id → on/off
  std::vector<int> client_fds_;                     // client fds
  std::unordered_map<int, std::string> client_ids_; // fd → client id
  std::vector<ActiveAlarm> active_alarms_;
  std::unordered_map<device_type, Limits> limits_;
  std::unordered_map<device_type, device_type> sensor_actuator_map_;

  void dispatch(int fd, const unpacked_message &msg);
  void handle_connect(int fd, const unpacked_message &msg);
  void handle_sensor_data(int fd, const unpacked_message &msg);
  void handle_actuator_ack(int fd, const unpacked_message &msg);
  void handle_read_request(int fd, const unpacked_message &msg);
  void handle_config(int fd, const unpacked_message &msg);
  void handle_disconnect(int fd);
  void remove_device(int fd);

  void check_hysteresis(const std::string &sensor_id, device_type sensor_type,
                        float value);
  void check_alarms(const std::string &sensor_id, device_type sensor_type,
                    float value);
  void resend_alarms();
  void send_alarms_to_clients();

  void send_connect_ack(int fd, uint16_t seq_ref, const std::string &dest_id,
                        connect_stat status, const char *reason = nullptr);
  void send_actuator_cmd(const std::string &actuator_id, actuator_state state,
                         actuator_cmd_reason reason);

  uint16_t next_seq();
  bool is_sensor_type(device_type t) const;
};
