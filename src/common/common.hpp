#pragma once

#include <cstdint>

// enum that defines the possible types of devices
enum class device_type : uint8_t {
  TEMPERATURE_SENSOR = 0x01,
  UMIDITY_SENSOR = 0x02,
  OXYGEN_SENSOR = 0x03,
  BPM_SENSOR = 0x04,
  HEATER_ACTUATOR = 0x10,
  HUMIDIFIER_ACTUATOR = 0x11,
  FAN_ACTUATOR = 0x12,
};

enum class disconnect_reason : uint8_t {
  NORMAL = 0x00,
  COMM_ERROR = 0x01,
  MAINTENANCE = 0x02,
};

#pragma pack(push, 1)

// connect message sent from either a sensor or an actuator to the manager to
// establish a connection.
//
// - d_type:          type of the device
//
// - device_id[16]:   ASCII identification of the device, must be right padded
// with "\0" to 16 bytes
//
// - description[32]: optional description for the device, must be right
// padded with "\0" to 32 bytes
struct connect {
  device_type d_type;
  char device_id[16];
  char description[32];
};

// message sent from any device to signal a disconnection for a given reason.
//
// - reason:  the reason for disconnection
//
// - message[32]: optional disconnection message, right padded with "\0" to 32
// bytes
struct disconnect {
  disconnect_reason reason;
  char message[32];
};

#pragma pack(pop) // Restore original compiler alignment settings
