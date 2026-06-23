#include "tof_net/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace tof_net {
namespace {

void throw_errno(const std::string &prefix) {
    throw std::runtime_error(prefix + ": " + std::strerror(errno));
}

sockaddr_in make_addr(const std::string &ip, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const std::string bind_ip = ip.empty() ? "0.0.0.0" : ip;
    if (inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 address: " + bind_ip);
    }
    return addr;
}

} // namespace

Socket::Socket() = default;
Socket::Socket(int fd) : fd_(fd) {}
Socket::~Socket() { close(); }

Socket::Socket(Socket &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
Socket &Socket::operator=(Socket &&other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

bool Socket::valid() const { return fd_ >= 0; }
int Socket::fd() const { return fd_; }

void Socket::close() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

void Socket::connect_to(const std::string &ip, uint16_t port, const std::string &bind_ip) {
    close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw_errno("socket");
    }
    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (!bind_ip.empty()) {
        sockaddr_in local = make_addr(bind_ip, 0);
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
            throw_errno("bind local ip");
        }
    }

    sockaddr_in remote = make_addr(ip, port);
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) < 0) {
        throw_errno("connect");
    }
}

void Socket::bind_listen(const std::string &ip, uint16_t port, int backlog) {
    close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw_errno("socket");
    }
    int yes = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        throw_errno("setsockopt");
    }
    sockaddr_in local = make_addr(ip, port);
    if (::bind(fd_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
        throw_errno("bind");
    }
    if (::listen(fd_, backlog) < 0) {
        throw_errno("listen");
    }
}

Socket Socket::accept_one(std::string *peer_ip, uint16_t *peer_port) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    int client = ::accept(fd_, reinterpret_cast<sockaddr *>(&peer), &len);
    if (client < 0) {
        throw_errno("accept");
    }
    if (peer_ip) {
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof(buf));
        *peer_ip = buf;
    }
    if (peer_port) {
        *peer_port = ntohs(peer.sin_port);
    }
    return Socket(client);
}

void Socket::send_all(const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = ::send(fd_, p + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("send");
        }
        if (n == 0) {
            throw std::runtime_error("send returned zero");
        }
        sent += static_cast<size_t>(n);
    }
}

void Socket::recv_all(void *data, size_t size) {
    auto *p = static_cast<uint8_t *>(data);
    size_t got = 0;
    while (got < size) {
        ssize_t n = ::recv(fd_, p + got, size - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno("recv");
        }
        if (n == 0) {
            throw std::runtime_error("peer closed connection");
        }
        got += static_cast<size_t>(n);
    }
}

void Socket::send_message(MessageType type, const void *payload, size_t payload_size) {
    auto bytes = pack_message(type, payload, payload_size);
    send_all(bytes.data(), bytes.size());
}

Message Socket::recv_message() {
    MessageHeader header;
    recv_all(&header, sizeof(header));
    if (header.magic != kMagic || header.version != kProtocolVersion) {
        throw std::runtime_error("invalid message header");
    }
    if (header.header_bytes != sizeof(MessageHeader)) {
        throw std::runtime_error("unsupported message header size");
    }
    if (header.payload_bytes > (uint64_t(1) << 34)) {
        throw std::runtime_error("payload too large");
    }
    Message msg;
    msg.type = static_cast<MessageType>(header.type);
    msg.payload.resize(static_cast<size_t>(header.payload_bytes));
    if (!msg.payload.empty()) {
        recv_all(msg.payload.data(), msg.payload.size());
    }
    return msg;
}

uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace tof_net
