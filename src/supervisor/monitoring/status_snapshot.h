#ifndef SVP_STATUS_SNAPSHOT_H
#define SVP_STATUS_SNAPSHOT_H

#include <windows.h>

#include <string>

struct service_runtime_t;

enum class runtime_health_t : unsigned long {
  unknown,
  disabled,
  starting,
  healthy,
  degraded,
  stopped,
};

struct status_snapshot_t {
  volatile long sequence = 0;
  unsigned long magic = 0;
  unsigned long version = 0;
  unsigned long wrapper_pid = 0;
  unsigned long child_pid = 0;
  unsigned long health = 0;
  unsigned long restart_count = 0;
  unsigned long cpu_percent = 0;
  unsigned long process_count = 0;
  unsigned long long process_started_ms = 0;
  unsigned long long memory_bytes = 0;
  unsigned long reload_requested = 0;
  unsigned long reload_completed = 0;
  unsigned long reload_result = 0;
};

bool create_status_snapshot(service_runtime_t *service);
void publish_status_snapshot(service_runtime_t *service);
void close_status_snapshot(service_runtime_t *service);
bool read_status_snapshot(const std::wstring &service_name,
                          status_snapshot_t *snapshot,
                          unsigned long *error = nullptr);
const wchar_t *runtime_health_text(runtime_health_t health);

#endif
