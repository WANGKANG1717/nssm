#include "console.h"

#include <windows.h>

bool console_attached() {
  return GetConsoleWindow() != nullptr;
}

void allocate_console(bool enabled) {
  if (enabled) AllocConsole();
}
