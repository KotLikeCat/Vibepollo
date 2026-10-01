/**
 * @file src/clipboard/watcher.h
 * @brief Polls the host clipboard and queues change notifications for streaming clients.
 */
#pragma once

namespace clipboard::watcher {
  /// Starts the watcher thread. No-op when clipboard_sync is disabled or unsupported.
  void start();
  /// Stops and joins the watcher thread.
  void stop();
}  // namespace clipboard::watcher
