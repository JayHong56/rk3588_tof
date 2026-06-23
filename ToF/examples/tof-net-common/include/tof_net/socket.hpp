#pragma once

#include "tof_net/protocol.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace tof_net {

class Socket {
  public:
    Socket();
    explicit Socket(int fd);
    ~Socket();

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept;
    Socket &operator=(Socket &&other) noexcept;

    bool valid() const;
    int fd() const;
    void close();

    void connect_to(const std::string &ip, uint16_t port, const std::string &bind_ip = {});
    void bind_listen(const std::string &ip, uint16_t port, int backlog = 1);
    Socket accept_one(std::string *peer_ip = nullptr, uint16_t *peer_port = nullptr);

    void send_all(const void *data, size_t size);
    void recv_all(void *data, size_t size);
    void send_message(MessageType type, const void *payload, size_t payload_size);
    Message recv_message();

  private:
    int fd_ = -1;
};

uint64_t now_ns();

} // namespace tof_net
