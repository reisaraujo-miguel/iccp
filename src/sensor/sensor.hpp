#pragma once

#include "../common/common.hpp"
#include <cstdint>

enum class sensor_stat : uint8_t {
  OK = 0x00,
  READING_ERROR = 0x01,
  OUT_OF_RANGE = 0x02,
};

#pragma pack(push, 1)

// sensor_data message sent from a sensor to the manager to report a reading.
//
// - device_id[16]: ASCII identification of the device, must be right padded
// with
// "\0" to 16 bytes
//
// - d_type: type of the device (device_type enum)
//
// - value: sensor reading (float)
//
// - unit[8]: unit of measurement, right padded with "\0" to 8 bytes
//
// - status: sensor_status enum (0x00 = OK, 0x01 = reading error, 0x02 = out
// of range)
struct sensor_data {
  char device_id[16];
  device_type d_type;
  float value;
  char unit[8];
  sensor_stat status;
};

#pragma pack(pop) // Restore original compiler alignment settings
