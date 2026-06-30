#pragma once

#include <cstdint>

enum class parameter_type : uint8_t {
  MIN = 0x01,
  MAX = 0x02,
  HYSTERESIS = 0x03,
  REPORTING_INTERVAL = 0x04,
};

#pragma pack(push, 1)

// read_request message sent from the manager to a sensor to request its current
// reading. The payload contains a list of sensor IDs to read from, each 16
// bytes long and right padded with "\0". If num_sensors is set to 0xFF, the
// sensor should return readings for all available sensors.
//
// - num_sensors: how many sensor IDs are included in the request (1 - 254, 0xFF
// = all sensors)
struct read_request {
  uint8_t num_sensors;
};

// config message sent from the client to the manager to configure its
// parameters. The payload contains a list of configuration entries, each 6
// bytes long:
//
// - device_type[1] (device_type enum)
// - param_type[1] (parameter identifier)
// - value[4] (float)
struct config {
  uint8_t num_params;
};

#pragma pack(pop) // Restore original compiler alignment settings
