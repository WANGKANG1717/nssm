#ifndef READINESS_H
#define READINESS_H

#include "config.h"

#include <windows.h>

#include <string>

enum class readiness_state_t {
  ready,
  timed_out,
  process_exited,
  cancelled,
  error,
};

struct readiness_result_t {
  readiness_state_t state = readiness_state_t::error;
  unsigned long error = ERROR_GEN_FAILURE;
  std::wstring detail;

  bool ready() const { return state == readiness_state_t::ready; }
};

readiness_result_t wait_for_readiness(
    const readiness_config_t &config, HANDLE application_process,
    const wchar_t *environment, const wchar_t *working_directory,
    HANDLE cancellation_event = nullptr,
    SERVICE_STATUS_HANDLE status_handle = nullptr,
    SERVICE_STATUS *service_status = nullptr,
    SRWLOCK *status_lock = nullptr);

#endif
