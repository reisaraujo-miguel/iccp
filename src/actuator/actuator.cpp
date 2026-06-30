#include "../actuator/actuator.hpp"
#include "../common/common.hpp"
#include "../manager/manager.hpp"
#include "../protocol/protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static constexpr const char *MANAGER_ID = "MANAGER\0\0\0\0\0\0\0\0\0";

static std::string pad16(const std::string &s) {
  std::string out(16, '\0');
  std::copy_n(s.begin(), std::min(s.size(), size_t{16}), out.begin());
  return out;
}

static uint32_t now_ms() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

int main(int argc, char **argv) {
  if (argc < 4) {
    std::cerr << "usage: actuator <device_id> <device_type> <server_ip> "
                 "[server_port=9000]"
              << std::endl;
    std::cerr << "  device_type: heater|humidifier|fan" << std::endl;
    return 1;
  }

  std::string dev_id = pad16(argv[1]);
  std::string dev_type_str = argv[2];
  std::string server_ip = argv[3];
  uint16_t server_port =
      (argc >= 5) ? static_cast<uint16_t>(std::stoi(argv[4])) : 9000;

  device_type d_type;
  if (dev_type_str == "heater") {
    d_type = device_type::HEATER_ACTUATOR;
  } else if (dev_type_str == "humidifier") {
    d_type = device_type::HUMIDIFIER_ACTUATOR;
  } else if (dev_type_str == "fan") {
    d_type = device_type::FAN_ACTUATOR;
  } else {
    std::cerr << "unknown device_type: " << dev_type_str << std::endl;
    return 1;
  }

  // connect to manager
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(server_port);
  inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr);

  if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    perror("connect");
    close(sock);
    return 1;
  }

  std::cout << "[actuator] connected to " << server_ip << ":" << server_port
            << std::endl;

  // send "CONNECT"
  struct connect conn{};
  conn.d_type = d_type;
  std::memcpy(conn.device_id, dev_id.data(), 16);
  std::memset(conn.description, 0, sizeof(conn.description));

  std::vector<uint8_t> conn_payload(sizeof(struct connect));
  std::memcpy(conn_payload.data(), &conn, sizeof(struct connect));

  auto conn_msg = create_message(message_type::CONNECT, 1, now_ms(), dev_id,
                                 MANAGER_ID, conn_payload);
  if (tcp_send(sock, conn_msg) < 0) {
    perror("send CONNECT");
    close(sock);
    return 1;
  }

  std::cout << "[actuator] CONNECT sent, waiting for ACK..." << std::endl;

  // wait for ACK
  unpacked_message ack_msg;
  try {
    ack_msg = tcp_receive(sock);
  } catch (const std::exception &e) {
    std::cerr << "[actuator] failed to receive CONNECT_ACK: " << e.what()
              << std::endl;
    close(sock);
    return 1;
  }

  if (ack_msg.header.type != message_type::CONNECT_ACK) {
    std::cerr << "[actuator] expected CONNECT_ACK, got "
              << static_cast<int>(ack_msg.header.type) << std::endl;
    close(sock);
    return 1;
  }

  connect_ack cack;
  if (ack_msg.payload.size() >= sizeof(cack))
    std::memcpy(&cack, ack_msg.payload.data(), sizeof(cack));

  if (cack.status != connect_stat::ACCEPTED) {
    std::cerr << "[actuator] connection rejected" << std::endl;
    close(sock);
    return 1;
  }

  std::cout << "[actuator] CONNECT_ACK accepted, waiting for commands..."
            << std::endl;

  actuator_state current_state = actuator_state::OFF;

  // listen for commands
  while (true) {
    unpacked_message msg;
    try {
      msg = tcp_receive(sock);
    } catch (const std::exception &e) {
      std::cerr << "[actuator] recv error: " << e.what() << std::endl;
      break;
    }

    if (msg.header.type != message_type::ACTUATOR_CMD) {
      std::cerr << "[actuator] unexpected msg type "
                << static_cast<int>(msg.header.type) << std::endl;
      continue;
    }

    if (msg.payload.size() < sizeof(actuator_cmd))
      continue;

    actuator_cmd cmd;
    std::memcpy(&cmd, msg.payload.data(), sizeof(cmd));

    current_state = cmd.command;

    std::cout << "[actuator] CMD: "
              << (cmd.command == actuator_state::ON ? "ON" : "OFF")
              << " reason=" << static_cast<int>(cmd.reason_code) << std::endl;

    // send ACK
    actuator_cmd_ack ackk{};
    ackk.sequence_ref = msg.header.sequence_number;
    ackk.status = actuator_cmd_stat::ACCEPTED;
    ackk.current_state = current_state;
    std::memset(ackk.reason, 0, sizeof(ackk.reason));
    std::memset(ackk.err_msg, 0, sizeof(ackk.err_msg));

    std::vector<uint8_t> payload(sizeof(actuator_cmd_ack));
    std::memcpy(payload.data(), &ackk, sizeof(actuator_cmd_ack));

    auto reply = create_message(message_type::ACTUATOR_ACK, 0, now_ms(), dev_id,
                                MANAGER_ID, payload);
    tcp_send(sock, reply);

    std::cout << "[actuator] ACK sent" << std::endl;
  }

  close(sock);
  return 0;
}
