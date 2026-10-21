#ifndef STATUS_QUERY_H
#define STATUS_QUERY_H

#include "failure.h"
#include "status_snapshot.h"

#include <string>

struct service_status_query_t {
  unsigned long windows_error = ERROR_SUCCESS;
  unsigned long state = SERVICE_STOPPED;
  unsigned long wrapper_pid = 0;
  unsigned long child_pid = 0;
  unsigned long restart_count = 0;
  unsigned long cpu_percent = 0;
  unsigned long process_count = 0;
  unsigned long long memory_bytes = 0;
  unsigned long long uptime_ms = 0;
  runtime_health_t health = runtime_health_t::unknown;
  bool runtime_available = false;
  bool failed = false;
  failure_record_t failure;
  unsigned long reload_requested = 0;
  unsigned long reload_completed = 0;
  unsigned long reload_result = 0;
  bool config_drift = false;
  bool restart_required = false;
};

bool query_service_runtime_status(const std::wstring &,
                                  service_status_query_t *);

#endif
