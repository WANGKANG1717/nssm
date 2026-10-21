#include "status_query.h"

#include "config_registration.h"
#include "win32_handle.h"

bool query_service_runtime_status(const std::wstring &name,
                                  service_status_query_t *result) {
  if (! result) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  *result = {};

  unique_service_handle manager(OpenSCManagerW(
      nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT));
  if (! manager) {
    result->windows_error = GetLastError();
    return false;
  }
  unique_service_handle service(
      OpenServiceW(manager.get(), name.c_str(), SERVICE_QUERY_STATUS));
  if (! service) {
    result->windows_error = GetLastError();
    return false;
  }

  SERVICE_STATUS_PROCESS status = {};
  unsigned long needed = 0;
  if (! QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<unsigned char *>(&status),
                             sizeof(status), &needed)) {
    result->windows_error = GetLastError();
    return false;
  }

  result->state = status.dwCurrentState;
  result->wrapper_pid = status.dwProcessId;
  status_snapshot_t snapshot = {};
  unsigned long snapshot_error = ERROR_SUCCESS;
  result->runtime_available =
      read_status_snapshot(name, &snapshot, &snapshot_error);
  if (result->runtime_available &&
      (! status.dwProcessId || snapshot.wrapper_pid != status.dwProcessId)) {
    result->runtime_available = false;
    snapshot_error = ERROR_INVALID_DATA;
  }
  if (result->runtime_available) {
    result->child_pid = snapshot.child_pid;
    result->restart_count = snapshot.restart_count;
    result->cpu_percent = snapshot.cpu_percent;
    result->memory_bytes = snapshot.memory_bytes;
    result->process_count = snapshot.process_count;
    result->health = static_cast<runtime_health_t>(snapshot.health);
    result->reload_requested = snapshot.reload_requested;
    result->reload_completed = snapshot.reload_completed;
    result->reload_result = snapshot.reload_result;
    if (snapshot.child_pid && snapshot.process_started_ms) {
      const unsigned long long now = GetTickCount64();
      result->uptime_ms = now >= snapshot.process_started_ms
                              ? now - snapshot.process_started_ms
                              : 0;
    }
  }
  result->windows_error = result->runtime_available
                              ? ERROR_SUCCESS
                              : snapshot_error;
  persistent_failure_t failure;
  unsigned long failure_error = ERROR_SUCCESS;
  if (! read_persistent_failure(name, &failure, &failure_error)) {
    result->windows_error = failure_error;
    return false;
  }
  result->failed = failure.failed;
  result->failure = failure.record;

  std::wstring config_path;
  unsigned long drift_error = ERROR_SUCCESS;
  unsigned long long applied_fingerprint = 0;
  unsigned long long current_fingerprint = 0;
  if (read_config_path(name, &config_path, &drift_error) &&
      config_file_fingerprint(config_path, &current_fingerprint,
                              &drift_error)) {
    const bool has_applied_fingerprint = read_config_fingerprint(
        name, &applied_fingerprint, &drift_error);
    result->config_drift = ! has_applied_fingerprint ||
                           current_fingerprint != applied_fingerprint;
    result->restart_required =
        result->runtime_available &&
        snapshot.config_fingerprint != current_fingerprint;
  }
  return true;
}
