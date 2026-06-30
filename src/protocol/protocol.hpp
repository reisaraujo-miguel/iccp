#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

constexpr uint16_t MAGIC = 0xCAFE;
constexpr uint8_t VERSION = 0x01;

enum class message_type : uint8_t {
  CONNECT = 0x01,
  CONNECT_ACK = 0x02,
  SENSOR_DATA = 0x03,
  ACTUATOR_CMD = 0x04,
  ACTUATOR_ACK = 0x05,
  READ_REQUEST = 0x06,
  READ_RESPONSE = 0x07,
  ALARM = 0x08,
  CONFIG = 0x09,
  CONFIG_ACK = 0x0A,
  DISCONNECT = 0x0B,
};

// Force the compiler to pack tightly on 1-byte boundaries
#pragma pack(push, 1)

struct message_header {
  uint16_t magic;
  uint8_t version;
  message_type type;
  uint16_t sequence_number;
  uint32_t timestamp;
  char src_id[16];  // must be right padded with "\0" to 16 bytes
  char dest_id[16]; // must be right padded with "\0" to 16 bytes
  uint16_t payload_length;
  uint16_t flags;
};

#pragma pack(pop) // Restore original compiler alignment settings

static_assert(sizeof(message_header) == 46, "message_header size mismatch!");

// Parsed message container returned by parse_message / tcp_receive.
struct unpacked_message {
  message_header header;
  std::vector<uint8_t> payload;
};

// Read one complete ICCP message from a TCP stream.
// Handles partial reads internally.
// Throws std::runtime_error on protocol violations or read failures.
unpacked_message tcp_receive(int sockfd);

// Build a complete ICCP message in a contiguous buffer (network byte order).
std::vector<uint8_t> create_message(message_type type, uint16_t sequence_number,
                                    uint32_t timestamp,
                                    const std::string &src_id,
                                    const std::string &dest_id,
                                    const std::vector<uint8_t> &payload);

// Reliably send a complete message over a TCP socket.
// Handles partial writes internally.  Returns the number of bytes sent
// on success, or -1 on error (errno is set).
ssize_t tcp_send(int sockfd, const std::vector<uint8_t> &message);
