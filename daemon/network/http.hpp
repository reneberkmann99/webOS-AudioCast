// SPDX-License-Identifier: MIT
#pragma once
#include "../common.hpp"
namespace ac {
struct Request {
  std::string method, path, authorization, body;
};
int listen_tcp(bool lan, unsigned port);
// Nonblocking incremental HTTP parser, hard cap and deadline imposed by caller.
bool parse_request(const std::string &, Request &);
bool authorized(const std::string &, const std::string &);
std::string response(unsigned code, const std::string &body,
                     const std::string &type = "application/json", bool cors = false);
void send_response(int, const std::string &, unsigned timeout_ms = 250);
class Engine;
void control_loop(Engine &, int server, const std::function<bool()> &stop);
} // namespace ac
