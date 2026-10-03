/**
 * @file src/platform/clipboard_agent.h
 * @brief Host side of the user-session clipboard agent (clipboard files, Mac to host).
 */
#pragma once

#include "src/clipboard/files/agent_protocol.h"

#include <functional>
#include <string>

namespace platf::clipboard_agent {
  /**
   * @brief Starts supervising tools\sunshine_clipboard_agent.exe as the logged-on user and connects a framed pipe.
   * @param on_message Called on the pipe worker thread for every message received from the agent.
   * @return true if the supervisor was started (the agent itself may still be absent or restarting).
   */
  bool start(std::function<void(const clipboard::files::agent::message &)> on_message);
  /// Stops supervision, disconnects and terminates the agent.
  void stop();
  /// True while the agent is launched and its pipe is connected.
  bool connected();
  /// Thread-safe, non-blocking enqueue of a complete frame (see agent_protocol.h). False if not connected or the queue is full.
  bool send(const std::string &frame);
}  // namespace platf::clipboard_agent
