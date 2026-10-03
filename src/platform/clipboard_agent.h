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
   * @param on_message Called on the pipe worker thread for every message received from the agent (hello/pong are consumed internally).
   * @param on_connection_changed Called with true after a valid hello (protocol version matches) and with false when the pipe
   *        breaks, the agent exits or stops responding. Everything queued for the dead agent is discarded, so on false the caller
   *        must fail its pending reads and re-publish or clear the current offer. May run on the pipe worker or supervisor thread.
   * @return true if the supervisor was started (the agent itself may still be absent or restarting).
   */
  bool start(std::function<void(const clipboard::files::agent::message &)> on_message, std::function<void(bool connected)> on_connection_changed = {});
  /// Stops supervision, disconnects and terminates the agent.
  void stop();
  /// True while the agent is launched and its pipe is connected.
  bool connected();
  /**
   * @brief Thread-safe, non-blocking enqueue of a complete frame (see agent_protocol.h).
   * @return true = queued, NOT necessarily delivered (a later on_connection_changed(false) means it was lost).
   *         false = dropped (not connected, or the 64 MiB queue cap would be exceeded: the newest frame is dropped);
   *         the caller must abort the affected read/offer.
   */
  bool send(const std::string &frame);
}  // namespace platf::clipboard_agent
