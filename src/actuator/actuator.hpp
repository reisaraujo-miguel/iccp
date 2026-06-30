#pragma once

#include <cstdint>

enum class actuator_state : uint8_t {
  OFF = 0x00,
  ON = 0x01,
};

enum class actuator_cmd_reason : uint8_t {
  OVER_MAX_LIMIT = 0x00,
  UNDER_MIN_LIMIT = 0x01,
  MANUAL_OVERRIDE = 0x02,
};

enum class actuator_cmd_stat : uint8_t {
  ACCEPTED = 0x00,
  ERROR = 0x01,
};

#pragma pack(push, 1)

// actuator_cmd message sent from the manager to an actuator to control its
// state.
//
// - device_id[16]: ASCII identification of the actuator, must be right padded
// with "\0" to 16 bytes
//
// - command: desired state of the actuator (actuator_state enum)
//
// - reason_code: optional reason code for the command (actuator_cmd_reason
// enum, 0x00 = no reason)
struct actuator_cmd {
  char device_id[16];
  actuator_state command;
  actuator_cmd_reason reason_code;
};

// actuator_cmd_ack message sent from an actuator to the manager to acknowledge
// a command.
//
// - sequence_ref:  the sequence number of the ACTUATOR_CMD message being
// acknowledged
//
// - status:        the status of the command (accepted or rejected)
// - current_state: the current state of the actuator (actuator_state enum)
//
// - reason[32]: optional rejection reason, right padded with "\0" to 32 bytes
//
// - err_msg[32]: optional error message, right padded with "\0" to 32 bytes
struct actuator_cmd_ack {
  uint16_t sequence_ref;
  actuator_cmd_stat status;
  actuator_state current_state;
  char reason[32];
  char err_msg[32];
};

#pragma pack(pop) // Restore original compiler alignment settings
