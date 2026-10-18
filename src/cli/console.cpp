#include "nssm.h"
#include "console.h"
#include "service.h"

/* See if we were launched from a console window. */
bool check_console() {
  return GetConsoleWindow() != nullptr;
}

void alloc_console(nssm_service_t *service) {
  if (service->no_console) return;

  AllocConsole();
}
