#include "logistics/central_server/http_upload_server.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <string>

namespace {

std::uint16_t AvailablePort() {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(socket_fd >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

    socklen_t address_size = sizeof(address);
    assert(getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
    close(socket_fd);
    return ntohs(address.sin_port);
}

}  // namespace

int main() {
    namespace server = logistics::central_server;

    server::Database database;
    const auto port = AvailablePort();
    server::HttpUploadServerConfig config;
    config.enabled = true;
    config.port = port;
    config.bearer_token = "valid-token";
    config.upload_root = "/tmp";
    server::HttpUploadServer upload_server(database, config);
    assert(upload_server.Start().ok());

    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(socket_fd >= 0);
    timeval timeout{ .tv_sec = 2, .tv_usec = 0 };
    assert(setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

    const std::string request =
        "POST /v1/uploads/images HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer invalid-token\r\n"
        "Content-Type: multipart/form-data; boundary=test\r\n"
        "Content-Length: 10485760\r\n"
        "Connection: close\r\n\r\n";
    assert(send(socket_fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));

    std::array<char, 1024> response{};
    const auto received = recv(socket_fd, response.data(), response.size(), 0);
    assert(received > 0);
    assert(std::string(response.data(), static_cast<std::size_t>(received)).find(" 401 ") != std::string::npos);

    close(socket_fd);
    upload_server.Stop();
    return 0;
}
