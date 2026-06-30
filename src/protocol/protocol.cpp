#include "protocol.hpp"
#include <algorithm>
#include <bit>
#include <concepts>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <vector>

template <std::integral T> constexpr T from_network_order(T value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return std::byteswap(value);
  }
  return value;
}

// Internal helper: read exactly `n` bytes from a TCP socket.
// Loops until all requested bytes arrive or a fatal condition occurs.
// Returns true on success, false on error or connection close.
static bool read_exact(int sockfd, void *buffer, std::size_t n) {
  std::size_t total_read = 0;
  auto *dst = static_cast<char *>(buffer);

  while (total_read < n) {
    ssize_t r = ::recv(sockfd, dst + total_read, n - total_read, 0);

    if (r < 0) {
      // errno is set by recv()
      return false;
    }
    if (r == 0) {
      // peer performed an orderly shutdown
      return false;
    }

    total_read += static_cast<std::size_t>(r);
  }

  return true;
}

unpacked_message tcp_receive(int sockfd) {
  message_header raw_header;
  if (!read_exact(sockfd, &raw_header, sizeof(raw_header))) {
    throw std::runtime_error("tcp_receive: failed to read message header");
  }

  raw_header.magic = from_network_order(raw_header.magic);
  raw_header.sequence_number = from_network_order(raw_header.sequence_number);
  raw_header.timestamp = from_network_order(raw_header.timestamp);
  raw_header.payload_length = from_network_order(raw_header.payload_length);
  raw_header.flags = from_network_order(raw_header.flags);

  if (raw_header.magic != MAGIC) {
    throw std::runtime_error("tcp_receive: invalid magic number");
  }
  if (raw_header.version != VERSION) {
    throw std::runtime_error("tcp_receive: unsupported protocol version");
  }

  unpacked_message msg;
  msg.header = raw_header;

  if (raw_header.payload_length > 0) {
    msg.payload.resize(raw_header.payload_length);
    if (!read_exact(sockfd, msg.payload.data(), raw_header.payload_length)) {
      throw std::runtime_error("tcp_receive: failed to read payload");
    }
  }

  return msg;
}

template <std::integral T> constexpr T to_network_order(T value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return std::byteswap(value); // Instantly swaps byte ordering
  }
  return value; // No-op if system is already Big-Endian
}

template <std::size_t N>
void copy_and_pad(const std::string &src, char (&dest)[N]) {
  // Determine how many actual characters we can safely copy
  const std::size_t copy_len = std::min(src.size(), N);

  std::copy_n(src.begin(), copy_len, dest);

  // Fill the remainder of the array with null terminators
  std::fill_n(dest + copy_len, N - copy_len, '\0');
}

std::vector<uint8_t> create_message(message_type type, uint16_t sequence_number,
                                    uint32_t timestamp,
                                    const std::string &src_id,
                                    const std::string &dest_id,
                                    const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> message(sizeof(message_header) + payload.size());

  // use placement new to build the header directly in the vector's memory
  message_header *header = ::new (message.data()) message_header();

  header->magic = to_network_order(MAGIC);
  header->version = VERSION;
  header->type = type;
  header->sequence_number = to_network_order(sequence_number);
  header->timestamp = to_network_order(timestamp);

  copy_and_pad(src_id, header->src_id);
  copy_and_pad(dest_id, header->dest_id);

  header->payload_length =
      to_network_order(static_cast<uint16_t>(payload.size()));
  header->flags = to_network_order(
      static_cast<uint16_t>(0)); // reserved for future use, set to 0

  // Copy the payload into the message
  if (!payload.empty()) {
    std::copy(payload.begin(), payload.end(),
              message.begin() + sizeof(message_header));
  }

  return message;
}

ssize_t tcp_send(int sockfd, const std::vector<uint8_t> &message) {
  std::size_t total_sent = 0;

  while (total_sent < message.size()) {
    ssize_t sent = ::send(sockfd, message.data() + total_sent,
                          message.size() - total_sent, 0);

    if (sent < 0) {
      // errno is already set by send()
      return -1;
    }

    total_sent += static_cast<std::size_t>(sent);
  }

  return static_cast<ssize_t>(total_sent);
}
