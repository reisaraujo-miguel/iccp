#include "../sensor/sensor.hpp"
#include "../common/common.hpp"
#include "../manager/manager.hpp"
#include "../protocol/protocol.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

static constexpr const char *MANAGER_ID = "MANAGER\0\0\0\0\0\0\0\0\0";

static uint32_t now_ms() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

static float generate_value(device_type d_type, float base) {
  static std::mt19937 rng(std::random_device{}());
  std::uniform_real_distribution<float> jitter(-0.5f, 0.5f);
  return base + jitter(rng);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    std::cerr << "usage: sensor <device_id> <device_type> <server_ip> "
                 "[server_port=9000] [base_value]"
              << std::endl;
    std::cerr << "  device_type: temperature|umidity|oxygen|bpm" << std::endl;
    return 1;
  }

  std::string dev_id = pad_n(argv[1], 16);
  std::string dev_type_str = argv[2];
  std::string server_ip = argv[3];

  uint16_t server_port =
      (argc >= 5) ? static_cast<uint16_t>(std::stoi(argv[4])) : 9000;

  float base_value = (argc >= 6) ? std::stof(argv[5]) : 37.0f;

  device_type d_type;
  std::string unit;

  if (dev_type_str == "temperature") {
    d_type = device_type::TEMPERATURE_SENSOR;
    unit = "Celsius";
  } else if (dev_type_str == "umidity") {
    d_type = device_type::UMIDITY_SENSOR;
    unit = "%";
  } else if (dev_type_str == "oxygen") {
    d_type = device_type::OXYGEN_SENSOR;
    unit = "%";
  } else if (dev_type_str == "bpm") {
    d_type = device_type::BPM_SENSOR;
    unit = "bpm";
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

  std::cout << "[sensor] connected to " << server_ip << ":" << server_port
            << std::endl;

  // send CONNECT
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

  std::cout << "[sensor] CONNECT sent, waiting for ACK..." << std::endl;

  // wait for ACK
  unpacked_message ack_msg;
  try {
    ack_msg = tcp_receive(sock);
  } catch (const std::exception &e) {
    std::cerr << "[sensor] failed to receive CONNECT_ACK: " << e.what()
              << std::endl;

    close(sock);
    return 1;
  }

  if (ack_msg.header.type != message_type::CONNECT_ACK) {
    std::cerr << "[sensor] expected CONNECT_ACK, got "
              << static_cast<int>(ack_msg.header.type) << std::endl;

    close(sock);
    return 1;
  }

  if (ack_msg.payload.size() < sizeof(connect_ack)) {
    std::cerr << "[sensor] CONNECT_ACK payload too short" << std::endl;
    close(sock);
    return 1;
  }

  connect_ack cack;
  std::memcpy(&cack, ack_msg.payload.data(), sizeof(cack));

  if (cack.status != connect_stat::ACCEPTED) {
    std::string reason(cack.reason, 32);
    std::cerr << "[sensor] connection rejected: " << reason << std::endl;

    close(sock);
    return 1;
  }

  uint16_t interval_ms = cack.interval_ms;
  std::cout << "[sensor] CONNECT_ACK accepted, interval=" << interval_ms << "ms"
            << std::endl;

  // periodic SENSOR_DATA
  uint16_t seq = 2;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));

    float value = generate_value(d_type, base_value);

    sensor_data sd{};

    std::memcpy(sd.device_id, dev_id.data(), 16);

    sd.d_type = d_type;
    sd.value = value;

    std::memcpy(sd.unit, pad_n(unit, 8).data(), 8);

    sd.status = sensor_stat::OK;

    std::vector<uint8_t> payload(sizeof(sensor_data));
    std::memcpy(payload.data(), &sd, sizeof(sensor_data));

    auto msg = create_message(message_type::SENSOR_DATA, seq++, now_ms(),
                              dev_id, MANAGER_ID, payload);

    if (tcp_send(sock, msg) < 0) {
      std::cerr << "[sensor] send error, exiting" << std::endl;
      break;
    }

    std::cout << "[sensor] data sent: value=" << value << std::endl;
  }

  close(sock);
  return 0;
}
