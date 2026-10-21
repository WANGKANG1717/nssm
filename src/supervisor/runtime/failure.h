#ifndef FAILURE_H
#define FAILURE_H

#include <string>

struct service_runtime_t;

enum class failure_kind_t : unsigned long {
  none = 0,
  process_exit,
  start_failure,
  readiness,
  health,
  watchdog,
  resource_memory,
  resource_cpu,
  resource_processes,
  lifecycle,
  restart_rate_limit,
  internal
};

struct failure_record_t {
  failure_kind_t kind = failure_kind_t::none;
  std::wstring phase;
  unsigned long exit_code = 0;
  unsigned long windows_error = 0;
  std::wstring message;
  unsigned long restart_attempts = 0;
  unsigned long long timestamp_100ns = 0;
};

const wchar_t *failure_kind_text(failure_kind_t);
failure_kind_t parse_failure_kind(const std::wstring &);

void reset_runtime_failure(service_runtime_t *);
bool record_runtime_failure(service_runtime_t *, failure_kind_t,
                            const wchar_t *, unsigned long, unsigned long,
                            const std::wstring &, unsigned long = 0);
failure_record_t runtime_failure(const service_runtime_t *);

#endif
