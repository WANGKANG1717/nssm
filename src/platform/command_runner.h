#ifndef COMMAND_RUNNER_H
#define COMMAND_RUNNER_H

#include "config.h"

#include <windows.h>

#include <string>

enum class command_state_t {
  succeeded,
  failed,
  timed_out,
  cancelled,
};

struct command_result_t {
  command_state_t state = command_state_t::failed;
  unsigned long windows_error = ERROR_SUCCESS;
  unsigned long exit_code = 0;
  std::wstring detail;

  bool succeeded() const { return state == command_state_t::succeeded; }
};

command_result_t run_command(const command_config_t &command,
                             const wchar_t *environment,
                             const wchar_t *working_directory,
                             HANDLE cancellation_event,
                             unsigned long timeout_ms);

#endif
