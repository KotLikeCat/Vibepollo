/**
 * @file tools/sunshine_clipboard_helper.cpp
 * @brief Reads the clipboard in the context of the user it is started as. Sunshine runs it with the
 *        interactive user's token because SYSTEM cannot see HTML/RTF/PNG clipboard formats.
 * @note Never writes anything but the payload to stdout/stderr (both are redirected to one file).
 *       Usage: `read <mask>` writes a clipboard bundle to stdout; `formats` prints the available mask.
 *       Exit codes: 0 ok, 2 bad arguments, 3 clipboard unavailable, 4 empty.
 */
#include "src/clipboard/bundle.h"
#include "src/platform/windows/clipboard_win.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
  if (argc < 2) {
    return 2;
  }
  const std::string command = argv[1];
  if (command == "formats" && argc == 2) {
    const auto mask = platf::clipboard_sync::local::available_formats();
    const std::string text = std::to_string(mask);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return 0;
  }
  if (command == "read" && argc == 3) {
    char *end = nullptr;
    const unsigned long mask = std::strtoul(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0') {
      return 2;
    }
    const bool com_ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    const auto items = platf::clipboard_sync::local::read(static_cast<std::uint32_t>(mask) & clipboard::format_all);
    if (!items) {
      if (com_ok) {
        CoUninitialize();
      }
      return 3;
    }
    const std::string bytes = clipboard::encode(*items);
    _setmode(_fileno(stdout), _O_BINARY);
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size();
    std::fflush(stdout);
    if (com_ok) {
      CoUninitialize();
    }
    return written ? 0 : 5;
  }
  return 2;
}
