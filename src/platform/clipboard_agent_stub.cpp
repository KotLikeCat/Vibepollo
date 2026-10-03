/**
 * @file src/platform/clipboard_agent_stub.cpp
 * @brief Clipboard agent stubs for platforms without an implementation.
 */
#include "src/platform/clipboard_agent.h"

namespace platf::clipboard_agent {
  bool start(std::function<void(const clipboard::files::agent::message &)>, std::function<void(bool)>) {
    return false;
  }

  void stop() {}

  bool connected() {
    return false;
  }

  bool send(const std::string &) {
    return false;
  }
}  // namespace platf::clipboard_agent
