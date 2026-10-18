#ifndef HEALTH_CHECK_H
#define HEALTH_CHECK_H

#include "config.h"

#include <windows.h>

#include <string>

struct health_probe_result_t {
  bool healthy = false;
  unsigned long error = ERROR_SUCCESS;
  std::wstring detail;
};

health_probe_result_t run_health_probe(const health_check_config_t &config,
                                       HANDLE application_process,
                                       const wchar_t *environment,
                                       const wchar_t *working_directory);

#endif
