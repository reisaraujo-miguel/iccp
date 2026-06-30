#include "../client/client.hpp"
#include "../common/common.hpp"
#include "../manager/manager.hpp"
#include "../protocol/protocol.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static constexpr const char *MANAGER_ID = "MANAGER\0\0\0\0\0\0\0\0\0";

static std::string id_to_string(const char (&id)[16]) { return {id, 16}; }

static uint32_t now_ms() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count()); // I hate cpp types soooo much!
}

static device_type parse_device_type(const std::string &s) {
  if (s == "temperature")
    return device_type::TEMPERATURE_SENSOR;
  if (s == "umidity")
    return device_type::UMIDITY_SENSOR;
  if (s == "oxygen")
    return device_type::OXYGEN_SENSOR;
  if (s == "bpm")
    return device_type::BPM_SENSOR;
  throw std::runtime_error("unknown device type: " + s);
}

static void print_usage() {
  std::cout << "Commands:\n"
            << "  read [sensor_id | all]  — request sensor readings\n"
            << "  config <type> <min> <max> <hyst>\n"
            << "  help\n"
            << "  quit\n"
            << "\n  Types: temperature, umidity, oxygen, bpm\n";
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: client <client_id> <server_ip> [server_port=9000]"
              << "\n";
    return 1;
  }

  std::string client_id = pad_n(argv[1], 16);
  std::string server_ip = argv[2];
  uint16_t server_port =
      (argc >= 4) ? static_cast<uint16_t>(std::stoi(argv[3])) : 9000;

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

  std::cout << "[client] connected to " << server_ip << ":" << server_port
            << "\n";
  print_usage();

  uint16_t seq = 1;

  // poll for both stdin and socket
  std::vector<pollfd> pfds;
  pfds.push_back({0, POLLIN, 0});    // stdin
  pfds.push_back({sock, POLLIN, 0}); // socket

  while (true) {
    int ready = poll(pfds.data(), pfds.size(), 100);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    // incoming messages from manager
    if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      if (pfds[1].revents & (POLLHUP | POLLERR)) {
        std::cerr << "[client] server disconnected" << "\n";
        break;
      }

      try {
        auto msg = tcp_receive(sock);

        switch (msg.header.type) {
        case message_type::READ_RESPONSE: {
          if (msg.payload.size() < sizeof(read_response))
            break;

          read_response rr;
          std::memcpy(&rr, msg.payload.data(), sizeof(rr));

          std::cout << "\n=== READ_RESPONSE status="
                    << static_cast<int>(rr.status)
                    << " entries=" << static_cast<int>(rr.num_entries)
                    << " ===\n";

          const uint8_t *base = msg.payload.data() + sizeof(read_response);
          size_t remain = msg.payload.size() - sizeof(read_response);

          for (int i = 0; i < rr.num_entries; ++i) {
            if (remain < 26)
              break;

            std::string sid(reinterpret_cast<const char *>(base), 16);
            uint8_t dt = base[16];

            float val;
            std::memcpy(&val, base + 17, 4);

            uint32_t ts;
            std::memcpy(&ts, base + 21, 4);

            uint8_t st = base[25];

            std::cout << "  [" << sid << "] type=" << static_cast<int>(dt)
                      << " val=" << val << " ts=" << ts
                      << " status=" << static_cast<int>(st) << "\n";

            base += 26;
            remain -= 26;
          }

          std::cout << "> ";
          std::cout.flush();
          break;
        }
        case message_type::ALARM: {
          if (msg.payload.size() < sizeof(struct alarm))
            break;

          struct alarm al;
          std::memcpy(&al, msg.payload.data(), sizeof(struct alarm));

          std::string sid(al.device_id, 16);

          std::cout << "\n*** ALARM #" << al.alarm_id
                    << " type=" << static_cast<int>(al.a_type)
                    << " severity=" << static_cast<int>(al.severity)
                    << " device=" << sid << " value=" << al.value
                    << " threshold=" << al.threshold << " ***\n> ";

          std::cout.flush();
          break;
        }
        case message_type::CONFIG_ACK: {
          if (msg.payload.size() < sizeof(config_ack))
            break;

          config_ack ca;
          std::memcpy(&ca, msg.payload.data(), sizeof(ca));

          std::cout << "\n--- CONFIG_ACK status=" << static_cast<int>(ca.status)
                    << " errors=" << static_cast<int>(ca.num_errors)
                    << " ---\n> ";

          std::cout.flush();
          break;
        }
        default:
          break;
        }
      } catch (const std::exception &e) {
        std::cerr << "[client] recv error: " << e.what() << "\n";
        break;
      }
    }

    // stdin commands
    if (pfds[0].revents & POLLIN) {
      std::string line;
      if (!std::getline(std::cin, line) || line.empty())
        continue;

      std::istringstream iss(line);
      std::string cmd;
      iss >> cmd;

      if (cmd == "quit" || cmd == "exit") {
        break;
      } else if (cmd == "help") {
        print_usage();
      } else if (cmd == "read") {
        std::string target;
        iss >> target;

        read_request rr{};
        std::vector<uint8_t> payload;
        payload.resize(sizeof(read_request));

        if (target.empty() || target == "all") {
          rr.num_sensors = 0xFF;
        } else {
          rr.num_sensors = 1;

          std::string sid = pad_n(target, 16);
          payload.insert(payload.end(),
                         reinterpret_cast<const uint8_t *>(sid.data()),
                         reinterpret_cast<const uint8_t *>(sid.data()) + 16);
        }

        std::memcpy(payload.data(), &rr, sizeof(rr));

        auto msg = create_message(message_type::READ_REQUEST, seq++, now_ms(),
                                  client_id, MANAGER_ID, payload);
        tcp_send(sock, msg);

        std::cout << "[client] READ_REQUEST sent" << "\n";
      } else if (cmd == "config") {
        std::string type_str;

        float min_v, max_v, hyst;
        iss >> type_str >> min_v >> max_v >> hyst;

        if (type_str.empty()) {
          std::cerr << "usage: config <type> <min> <max> <hyst>\n";
          continue;
        }

        device_type dt;
        try {
          dt = parse_device_type(type_str);
        } catch (...) {
          std::cerr << "unknown type: " << type_str << "\n";
          continue;
        }

        // build payload: config header + 3 entries (min, max, hysteresis)
        config cfg{};

        cfg.num_params = 3;

        std::vector<uint8_t> payload(sizeof(config));
        std::memcpy(payload.data(), &cfg, sizeof(cfg));

        // helper to add a param entry (6 bytes: type, param, value)
        auto add_param = [&](parameter_type pt, float v) {
          uint8_t entry[6];
          entry[0] = static_cast<uint8_t>(dt);
          entry[1] = static_cast<uint8_t>(pt);
          std::memcpy(entry + 2, &v, 4);
          payload.insert(payload.end(), entry, entry + 6);
        };

        add_param(parameter_type::MIN, min_v);
        add_param(parameter_type::MAX, max_v);
        add_param(parameter_type::HYSTERESIS, hyst);

        auto msg = create_message(message_type::CONFIG, seq++, now_ms(),
                                  client_id, MANAGER_ID, payload);

        tcp_send(sock, msg);

        std::cout << "[client] CONFIG sent: " << type_str << " min=" << min_v
                  << " max=" << max_v << " hyst=" << hyst << "\n";
      } else {
        std::cout << "unknown command: " << cmd << "\n";
        print_usage();
      }
    }
  }

  close(sock);
  return 0;
}
