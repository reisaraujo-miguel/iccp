#include "manager.hpp"

#include "../actuator/actuator.hpp"
#include "../client/client.hpp"
#include "../common/common.hpp"
#include "../protocol/protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

static constexpr const char *MANAGER_ID = "MANAGER\0\0\0\0\0\0\0\0\0"; // 16b

static std::string id_to_string(const char (&id)[16]) { return {id, 16}; }

static uint32_t now_ms() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// default configuration per device type
static std::unordered_map<device_type, Limits> default_limits() {
  return {
      {device_type::TEMPERATURE_SENSOR, {35.0f, 38.0f, 0.5f}},
      {device_type::UMIDITY_SENSOR, {40.0f, 70.0f, 2.0f}},
      {device_type::OXYGEN_SENSOR, {18.0f, 25.0f, 1.0f}},
      {device_type::BPM_SENSOR, {100.0f, 160.0f, 5.0f}},
  };
}

// sensor → actuator mapping (temperature controls fan for cooling)
static std::unordered_map<device_type, device_type> default_mapping() {
  return {
      {device_type::TEMPERATURE_SENSOR, device_type::FAN_ACTUATOR},
  };
}

Manager::Manager(uint16_t port)
    : port_(port), seq_(0), limits_(default_limits()),
      sensor_actuator_map_(default_mapping()) {}

Manager::~Manager() {
  if (listen_fd_ >= 0)
    close(listen_fd_);
}

void Manager::run() {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    perror("socket");
    return;
  }

  int opt = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    perror("bind");
    return;
  }

  if (listen(listen_fd_, 16) < 0) {
    perror("listen");
    return;
  }

  std::cout << "[manager] listening on 0.0.0.0:" << port_ << "\n";

  std::vector<pollfd> pfds;
  pfds.push_back({listen_fd_, POLLIN, 0});

  while (true) {
    int ready = poll(pfds.data(), pfds.size(), 5000);
    if (ready < 0) {
      if (errno == EINTR)
        continue;

      perror("poll");
      break;
    }

    // new connections
    if (!pfds.empty() && (pfds[0].revents & POLLIN)) {
      sockaddr_in client_addr{};
      socklen_t len = sizeof(client_addr);

      int cfd =
          accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_addr), &len);

      if (cfd >= 0) {
        pfds.push_back({cfd, POLLIN, 0});

        std::cout << "[manager] new connection fd=" << cfd << " from "
                  << inet_ntoa(client_addr.sin_addr) << ":"
                  << ntohs(client_addr.sin_port) << "\n";
      }

      --ready;
    }

    // data from connected peers
    for (size_t i = 1; i < pfds.size() && ready > 0; /**/) {
      if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        --ready;

        int fd = pfds[i].fd;
        if (pfds[i].revents & (POLLHUP | POLLERR)) {
          handle_disconnect(fd);
          close(fd);

          remove_device(fd);
          pfds.erase(pfds.begin() + static_cast<long>(i));

          continue;
        }

        try {
          auto msg = tcp_receive(fd);
          dispatch(fd, msg);
        } catch (const std::exception &e) {
          std::cerr << "[manager] recv error fd=" << fd << ": " << e.what()
                    << "\n";

          handle_disconnect(fd);
          close(fd);

          remove_device(fd);
          pfds.erase(pfds.begin() + static_cast<long>(i));

          continue;
        }
      }
      ++i;
    }

    // periodic alarm re-check
    resend_alarms();
  }
}

void Manager::dispatch(int fd, const unpacked_message &msg) {
  switch (msg.header.type) {
  case message_type::CONNECT:
    handle_connect(fd, msg);
    break;
  case message_type::SENSOR_DATA:
    handle_sensor_data(fd, msg);
    break;
  case message_type::ACTUATOR_ACK:
    handle_actuator_ack(fd, msg);
    break;
  case message_type::READ_REQUEST:
    handle_read_request(fd, msg);
    break;
  case message_type::CONFIG:
    handle_config(fd, msg);
    break;
  case message_type::DISCONNECT:
    handle_disconnect(fd);
    break;
  default:
    std::cerr << "[manager] unexpected msg type "
              << static_cast<int>(msg.header.type) << " fd=" << fd << "\n";
  }
}

void Manager::handle_connect(int fd, const unpacked_message &msg) {
  if (msg.payload.size() < sizeof(struct connect)) {
    std::cerr << "[manager] CONNECT payload too short" << "\n";
    return;
  }

  struct connect conn;
  std::memcpy(&conn, msg.payload.data(), sizeof(conn));

  std::string dev_id = id_to_string(conn.device_id);
  device_type d_type = conn.d_type;

  for (auto &[_, dev] : devices_) {
    if (dev.id == dev_id) {
      send_connect_ack(fd, msg.header.sequence_number, dev_id,
                       connect_stat::REJECT_DUP_ID, "duplicate ID");
      return;
    }
  }

  DeviceInfo dev;

  dev.fd = fd;
  dev.id = dev_id;
  dev.type = d_type;

  devices_[fd] = dev;

  std::cout << "[manager] device \"" << dev_id
            << "\" type=" << static_cast<int>(d_type) << " connected fd=" << fd
            << "\n";

  send_connect_ack(fd, msg.header.sequence_number, dev_id,
                   connect_stat::ACCEPTED);
}

void Manager::handle_sensor_data(int fd, const unpacked_message &msg) {
  if (msg.payload.size() < sizeof(sensor_data))
    return;

  sensor_data sd;
  std::memcpy(&sd, msg.payload.data(), sizeof(sd));

  std::string dev_id = id_to_string(sd.device_id);
  device_type d_type = sd.d_type;

  Reading r;

  r.value = sd.value;
  r.timestamp = msg.header.timestamp;
  r.status = sd.status;
  r.unit.assign(sd.unit, 8);
  r.type = d_type;

  last_readings_[dev_id] = r;

  std::cout << "[manager] reading \"" << dev_id
            << "\" type=" << static_cast<int>(d_type) << " val=" << sd.value
            << "\n";

  check_hysteresis(dev_id, d_type, sd.value);
  check_alarms(dev_id, d_type, sd.value);
}

void Manager::handle_actuator_ack(int fd, const unpacked_message &msg) {
  if (msg.payload.size() < sizeof(actuator_cmd_ack))
    return;

  actuator_cmd_ack ack;
  std::memcpy(&ack, msg.payload.data(), sizeof(ack));

  auto it = devices_.find(fd);
  if (it == devices_.end())
    return;

  if (ack.status == actuator_cmd_stat::ACCEPTED) {
    actuator_states_[it->second.id] = (ack.current_state == actuator_state::ON);
  }

  std::cout << "[manager] actuator \"" << it->second.id
            << "\" ack seq=" << ack.sequence_ref
            << " st=" << static_cast<int>(ack.status)
            << " cur=" << static_cast<int>(ack.current_state) << "\n";
}

void Manager::handle_read_request(int fd, const unpacked_message &msg) {
  if (msg.payload.size() < sizeof(read_request))
    return;

  read_request req;
  std::memcpy(&req, msg.payload.data(), sizeof(req));

  if (std::find(client_fds_.begin(), client_fds_.end(), fd) ==
      client_fds_.end()) {

    client_fds_.push_back(fd);
    client_ids_[fd] = id_to_string(msg.header.src_id);
  }

  std::vector<std::string> requested;
  if (req.num_sensors == 0xFF) {
    for (auto &[id, _] : last_readings_) {
      requested.push_back(id);
    }
  } else {
    size_t off = sizeof(read_request);

    for (int i = 0; i < req.num_sensors; ++i) {
      if (off + 16 > msg.payload.size())
        break;

      char buf[16];
      std::memcpy(buf, msg.payload.data() + off, 16);

      requested.push_back({buf, 16});
      off += 16;
    }
  }

  std::vector<uint8_t> resp_payload;
  resp_payload.resize(sizeof(read_response));

  uint8_t entries = 0;
  read_resp_stat overall = read_resp_stat::OK;

  for (auto &sid : requested) {
    auto it = last_readings_.find(sid);

    uint8_t entry[26] = {};
    std::memcpy(entry, sid.data(), 16);

    if (it != last_readings_.end()) {
      entry[16] = static_cast<uint8_t>(it->second.type);

      std::memcpy(entry + 17, &it->second.value, 4);
      std::memcpy(entry + 21, &it->second.timestamp, 4);

      entry[25] = static_cast<uint8_t>(it->second.status);
    } else {
      entry[16] = 0;

      float zero = 0;
      std::memcpy(entry + 17, &zero, 4);

      uint32_t tzero = 0;
      std::memcpy(entry + 21, &tzero, 4);

      entry[25] = static_cast<uint8_t>(sensor_stat::READING_ERROR);

      overall = read_resp_stat::SENSOR_NOT_FOUND;
    }

    resp_payload.insert(resp_payload.end(), entry, entry + 26);
    ++entries;
  }

  if (entries == 0)
    overall = read_resp_stat::NO_DATA;

  auto *rr = reinterpret_cast<read_response *>(resp_payload.data());

  rr->sequence_ref = msg.header.sequence_number;
  rr->status = overall;
  rr->num_entries = entries;

  auto message =
      create_message(message_type::READ_RESPONSE, next_seq(), now_ms(),
                     MANAGER_ID, id_to_string(msg.header.src_id), resp_payload);

  tcp_send(fd, message);

  std::cout << "[manager] read_response → " << static_cast<int>(entries)
            << " entries" << "\n";
}

void Manager::handle_config(int fd, const unpacked_message &msg) {
  if (msg.payload.size() < sizeof(config))
    return;

  config cfg;
  std::memcpy(&cfg, msg.payload.data(), sizeof(cfg));

  std::vector<uint8_t> errors;
  bool any_error = false;

  const uint8_t *base = msg.payload.data() + sizeof(config);
  size_t remain = msg.payload.size() - sizeof(config);

  for (int i = 0; i < cfg.num_params; ++i) {
    if (remain < 6)
      break;

    auto d_type = static_cast<device_type>(base[0]);
    auto p_type = static_cast<parameter_type>(base[1]);

    float val;
    std::memcpy(&val, base + 2, 4);

    if (!is_sensor_type(d_type)) {
      errors.push_back(static_cast<uint8_t>(i));
      errors.push_back(0x02);

      any_error = true;
    } else {
      auto &lim = limits_[d_type];

      switch (p_type) {
      case parameter_type::MIN:
        lim.min = val;
        break;
      case parameter_type::MAX:
        lim.max = val;
        break;
      case parameter_type::HYSTERESIS:
        lim.hysteresis = val;
        break;
      default:
        errors.push_back(static_cast<uint8_t>(i));
        errors.push_back(0x02);

        any_error = true;
        break;
      }
    }

    base += 6;
    remain -= 6;
  }

  std::vector<uint8_t> ack_payload;
  ack_payload.resize(sizeof(config_ack));

  auto *ca = reinterpret_cast<config_ack *>(ack_payload.data());

  ca->sequence_ref = msg.header.sequence_number;
  ca->status = any_error ? config_stat::REJECTED : config_stat::ACCEPTED;
  ca->num_errors = static_cast<uint8_t>(errors.size() / 2);

  ack_payload.insert(ack_payload.end(), errors.begin(), errors.end());

  auto message =
      create_message(message_type::CONFIG_ACK, next_seq(), now_ms(), MANAGER_ID,
                     id_to_string(msg.header.src_id), ack_payload);

  tcp_send(fd, message);

  std::cout << "[manager] config_ack → " << static_cast<int>(ca->status)
            << "\n";
}

void Manager::check_hysteresis(const std::string &sensor_id,
                               device_type sensor_type, float value) {

  auto map_it = sensor_actuator_map_.find(sensor_type);
  if (map_it == sensor_actuator_map_.end())
    return;

  device_type act_type = map_it->second;

  auto lim_it = limits_.find(sensor_type);
  if (lim_it == limits_.end())
    return;

  const auto &lim = lim_it->second;

  std::string actuator_id;
  for (auto &[_, dev] : devices_) {
    if (dev.type == act_type) {
      actuator_id = dev.id;
      break;
    }
  }
  if (actuator_id.empty())
    return;

  bool currently_on = actuator_states_[actuator_id];
  bool should_be_on;

  if (value > lim.max + lim.hysteresis) {
    should_be_on = true;
  } else if (value <= lim.max) {
    should_be_on = false;
  } else {
    should_be_on = currently_on;
  }

  if (should_be_on != currently_on) {
    std::cout << "[manager] hysteresis: " << sensor_id << " val=" << value
              << " max=" << lim.max << " hyst=" << lim.hysteresis << " → "
              << (should_be_on ? "ON" : "OFF") << " act=" << actuator_id
              << "\n";

    send_actuator_cmd(actuator_id,
                      should_be_on ? actuator_state::ON : actuator_state::OFF,
                      actuator_cmd_reason::OVER_MAX_LIMIT);
  }
}

void Manager::check_alarms(const std::string &sensor_id,
                           device_type sensor_type, float value) {

  if (sensor_type != device_type::OXYGEN_SENSOR &&
      sensor_type != device_type::BPM_SENSOR) {
    return;
  }

  auto lim_it = limits_.find(sensor_type);
  if (lim_it == limits_.end())
    return;

  const auto &lim = lim_it->second;

  if (value < lim.min) {
    alarm_type a_type = (sensor_type == device_type::OXYGEN_SENSOR)
                            ? alarm_type::CRITICAL_OXYGENATION
                            : alarm_type::CRITICAL_BPM;

    bool exists = false;
    for (auto &al : active_alarms_) {
      if (al.device_id == sensor_id && al.a_type == a_type) {
        exists = true;

        al.value = value;
        al.threshold = lim.min;
        al.timestamp = now_ms();

        break;
      }
    }

    if (!exists) {
      ActiveAlarm al;

      al.alarm_id = next_alarm_id_++;
      al.a_type = a_type;
      al.severity = alarm_severity::CRITICAL;
      al.device_id = sensor_id;
      al.value = value;
      al.threshold = lim.min;
      al.timestamp = now_ms();

      active_alarms_.push_back(al);
    }

    std::cout << "[manager] ALARM! " << sensor_id << " val=" << value
              << " < min=" << lim.min << "\n";
  } else {
    active_alarms_.erase(std::remove_if(active_alarms_.begin(),
                                        active_alarms_.end(),
                                        [&](const ActiveAlarm &a) {
                                          return a.device_id == sensor_id;
                                        }),
                         active_alarms_.end());
  }
}

void Manager::resend_alarms() {
  if (!active_alarms_.empty())
    send_alarms_to_clients();
}

void Manager::send_alarms_to_clients() {
  for (auto &alarm : active_alarms_) {
    struct alarm al_msg{};

    al_msg.alarm_id = alarm.alarm_id;
    al_msg.a_type = alarm.a_type;
    al_msg.severity = alarm.severity;

    std::memcpy(al_msg.device_id, alarm.device_id.data(), 16);

    al_msg.value = alarm.value;
    al_msg.threshold = alarm.threshold;
    al_msg.timestamp = alarm.timestamp;

    std::memset(al_msg.message, 0, sizeof(al_msg.message));

    std::vector<uint8_t> payload(sizeof(struct alarm));
    std::memcpy(payload.data(), &al_msg, sizeof(struct alarm));

    for (int cfd : client_fds_) {
      std::string dest = "CLIENT\0\0\0\0\0\0\0\0\0\0";

      auto it = client_ids_.find(cfd);
      if (it != client_ids_.end())
        dest = it->second;

      auto msg = create_message(message_type::ALARM, next_seq(), now_ms(),
                                MANAGER_ID, dest, payload);

      tcp_send(cfd, msg);
    }
  }
}

void Manager::send_actuator_cmd(const std::string &actuator_id,
                                actuator_state state,
                                actuator_cmd_reason reason) {
  int act_fd = -1;
  for (auto &[fd, dev] : devices_) {
    if (dev.id == actuator_id) {
      act_fd = fd;
      break;
    }
  }
  if (act_fd < 0)
    return;

  actuator_cmd cmd{};

  std::memcpy(cmd.device_id, actuator_id.data(), 16);
  cmd.command = state;
  cmd.reason_code = reason;

  std::vector<uint8_t> payload(sizeof(actuator_cmd));
  std::memcpy(payload.data(), &cmd, sizeof(actuator_cmd));

  auto msg = create_message(message_type::ACTUATOR_CMD, next_seq(), now_ms(),
                            MANAGER_ID, actuator_id, payload);

  tcp_send(act_fd, msg);

  actuator_states_[actuator_id] = (state == actuator_state::ON);
}

void Manager::send_connect_ack(int fd, uint16_t seq_ref,
                               const std::string &dest_id, connect_stat status,
                               const char *reason) {
  connect_ack ack{};

  ack.sequence_ref = seq_ref;
  ack.status = status;
  ack.interval_ms = 1000;

  std::memset(ack.reason, 0, sizeof(ack.reason));

  if (reason)
    std::strncpy(ack.reason, reason, sizeof(ack.reason) - 1);

  std::vector<uint8_t> payload(sizeof(connect_ack));
  std::memcpy(payload.data(), &ack, sizeof(connect_ack));

  auto msg = create_message(message_type::CONNECT_ACK, next_seq(), now_ms(),
                            MANAGER_ID, dest_id, payload);

  tcp_send(fd, msg);

  std::cout << "[manager] connect_ack → " << dest_id
            << " st=" << static_cast<int>(status) << " iv=" << ack.interval_ms
            << "ms" << "\n";
}

void Manager::handle_disconnect(int fd) {
  auto it = devices_.find(fd);
  if (it != devices_.end())
    std::cout << "[manager] device \"" << it->second.id << "\" disconnected"
              << "\n";

  client_fds_.erase(std::remove(client_fds_.begin(), client_fds_.end(), fd),
                    client_fds_.end());

  client_ids_.erase(fd);
}

void Manager::remove_device(int fd) { devices_.erase(fd); }

uint16_t Manager::next_seq() { return seq_++; }

bool Manager::is_sensor_type(device_type t) const {
  return t == device_type::TEMPERATURE_SENSOR ||
         t == device_type::UMIDITY_SENSOR || t == device_type::OXYGEN_SENSOR ||
         t == device_type::BPM_SENSOR;
}

int main(int argc, char **argv) {
  uint16_t port = 9000;
  if (argc >= 2)
    port = static_cast<uint16_t>(std::stoi(argv[1]));

  Manager mgr(port);
  mgr.run();

  return 0;
}
