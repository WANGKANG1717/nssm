#include "console.h"

#include <windows.h>

bool console_attached() {
  return GetConsoleWindow() != nullptr;
}

void allocate_console(bool enabled) {
  if (enabled) AllocConsole();
}

bool write_standard_output_bytes(const void *data, size_t size,
                                 unsigned long *error) {
  if (error) *error = ERROR_SUCCESS;
  if (! data && size) {
    if (error) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (! output || output == INVALID_HANDLE_VALUE) {
    if (error) *error = GetLastError();
    return false;
  }
  const auto *bytes = static_cast<const unsigned char *>(data);
  while (size) {
    const unsigned long chunk = static_cast<unsigned long>(
        size > MAXDWORD ? MAXDWORD : size);
    unsigned long written = 0;
    if (! WriteFile(output, bytes, chunk, &written, nullptr) || ! written) {
      if (error) *error = GetLastError();
      return false;
    }
    bytes += written;
    size -= written;
  }
  return true;
}
