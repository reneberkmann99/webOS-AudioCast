// SPDX-License-Identifier: MIT
#include "http.hpp"
#include "../engine.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <sstream>
namespace ac {
int listen_tcp(bool lan, unsigned port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    throw std::runtime_error("socket creation failed");
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(port));
  a.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
  if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0 || listen(fd, 12) < 0) {
    close(fd);
    throw std::runtime_error("cannot bind port " + std::to_string(port) + ": " + strerror(errno));
  }
  return fd;
}
bool parse_request(const std::string &s, Request &r) {
  if (s.size() > 16384)
    throw std::runtime_error("HTTP request too large");
  auto end = s.find("\r\n\r\n");
  if (end == std::string::npos)
    return false;
  std::istringstream in(s.substr(0, end + 2));
  std::string line;
  std::getline(in, line);
  std::istringstream first(line);
  std::string version, extra;
  first >> r.method >> r.path >> version;
  if (!first || first >> extra || version != "HTTP/1.1" || r.path.empty() || r.path[0] != '/')
    throw std::runtime_error("bad HTTP request");
  std::map<std::string, std::string> h;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    auto c = line.find(':');
    if (c == std::string::npos || c == 0)
      throw std::runtime_error("bad HTTP header");
    std::string k = line.substr(0, c), v = line.substr(c + 1);
    for (char &x : k)
      x = char(std::tolower(static_cast<unsigned char>(x)));
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
      v.erase(v.begin());
    if (h.count(k))
      throw std::runtime_error("duplicate HTTP header");
    h[k] = v;
  }
  if (h.count("transfer-encoding"))
    throw std::runtime_error("chunked requests not supported");
  size_t n = 0;
  if (h.count("content-length")) {
    std::string v = h["content-length"];
    if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos || v.size() > 5)
      throw std::runtime_error("bad content length");
    n = std::stoul(v);
    if (n > 8192)
      throw std::runtime_error("body too large");
  }
  if (s.size() < end + 4 + n)
    return false;
  if (s.size() != end + 4 + n)
    throw std::runtime_error("pipelined requests not supported");
  r.authorization = h["authorization"];
  r.body = s.substr(end + 4, n);
  return true;
}
bool authorized(const std::string &v, const std::string &t) {
  const std::string want = "Bearer " + t;
  size_t n = std::max(v.size(), want.size());
  unsigned diff = unsigned(v.size() ^ want.size());
  for (size_t i = 0; i < n; ++i)
    diff |= unsigned((i < v.size() ? v[i] : 0) ^ (i < want.size() ? want[i] : 0));
  return diff == 0;
}
std::string response(unsigned code, const std::string &body, const std::string &type, bool cors) {
  std::string status = code == 200   ? "OK"
                       : code == 202 ? "Accepted"
                       : code == 204 ? "No Content"
                       : code == 400 ? "Bad Request"
                       : code == 401 ? "Unauthorized"
                       : code == 404 ? "Not Found"
                       : code == 409 ? "Conflict"
                       : code == 503 ? "Service Unavailable"
                                     : "Error";
  return "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\nContent-Type: " + type +
         "\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\nCache-Control: no-store\r\n" +
         (cors ? "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, "
                 "OPTIONS\r\nAccess-Control-Allow-Headers: Authorization, Content-Type\r\n"
               : "") +
         "\r\n" + body;
}
void send_response(int fd, const std::string &s, unsigned timeout) {
  size_t n = 0;
  auto start = Clock::now();
  while (n < s.size() && seconds(start) * 1000 < timeout) {
    ssize_t k = send(fd, s.data() + n, s.size() - n, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (k > 0)
      n += size_t(k);
    else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      pollfd p{fd, POLLOUT, 0};
      poll(&p, 1, 10);
    } else
      break;
  }
}
void control_loop(Engine &e, int server, const std::function<bool()> &stop) {
  while (!stop()) {
    pollfd p{server, POLLIN, 0};
    if (poll(&p, 1, 100) <= 0)
      continue;
    int fd = accept(server, nullptr, nullptr);
    if (fd < 0)
      continue;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::string data;
    Request r;
    auto born = Clock::now();
    try {
      bool ready = false;
      while (seconds(born) < .3 && !stop()) {
        char b[4096];
        ssize_t n = recv(fd, b, sizeof(b), MSG_DONTWAIT);
        if (n > 0) {
          data.append(b, size_t(n));
          if (parse_request(data, r)) {
            ready = true;
            break;
          }
        } else if (n == 0)
          break;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
          break;
        pollfd q{fd, POLLIN, 0};
        poll(&q, 1, 10);
      }
      if (!ready)
        throw std::runtime_error("incomplete HTTP request");
      if (r.method == "OPTIONS")
        send_response(fd, response(204, "", "text/plain", true));
      else if (!authorized(r.authorization, e.control_token))
        send_response(
            fd, response(401, "{\"error\":\"authentication required\"}", "application/json", true));
      else {
        Json body = r.body.empty() ? Json::object() : Json::parse(r.body);
        Json out = e.route(r.method, r.path, body);
        send_response(fd, response(200, out.dump(), "application/json", true));
      }
    } catch (const std::exception &x) {
      send_response(fd,
                    response(400, Json({{"error", x.what()}}).dump(), "application/json", true));
    }
    close(fd);
  }
}
} // namespace ac
