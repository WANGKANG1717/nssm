#include "service.h"

#include "config.h"
#include "event.h"
#include "health_monitor.h"
#include "messages.h"
#include "service_io.h"
#include "notification.h"
#include "resource_monitor.h"
#include "recovery.h"
#include "service_constants.h"
#include "service_process_wait.h"
#include "status_snapshot.h"

void set_service_runtime_defaults(service_runtime_t *service) {
  if (! service) return;

  InitializeSRWLock(&service->status_snapshot_lock);
  InitializeSRWLock(&service->status_lock);
  InitializeSRWLock(&service->wait_lock);
  service->stdin_sharing = SVP_STDIN_SHARING;
  service->stdin_disposition = SVP_STDIN_DISPOSITION;
  service->stdin_flags = SVP_STDIN_FLAGS;
  service->stdout_sharing = SVP_STDOUT_SHARING;
  service->stdout_disposition = SVP_STDOUT_DISPOSITION;
  service->stdout_flags = SVP_STDOUT_FLAGS;
  service->stderr_sharing = SVP_STDERR_SHARING;
  service->stderr_disposition = SVP_STDERR_DISPOSITION;
  service->stderr_flags = SVP_STDERR_FLAGS;
  service->kill_console_delay = kKillConsoleGracePeriodMs;
  service->kill_window_delay = kKillWindowGracePeriodMs;
  service->kill_threads_delay = kKillThreadsGracePeriodMs;
  service->kill_process_tree = true;
}

service_runtime_t *allocate_service_runtime() {
  auto *service = static_cast<service_runtime_t *>(
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(service_runtime_t)));
  if (! service) {
    log_event(EVENTLOG_ERROR_TYPE, SVP_EVENT_OUT_OF_MEMORY, L"service",
              L"allocate_service_runtime()", 0);
  }
  return service;
}

void cleanup_service_runtime(service_runtime_t *service) {
  if (! service) return;
  unregister_service_process_wait(service, true);
  stop_resource_monitor(service);
  stop_health_monitor(service);
  stop_notification_listener(service);
  close_status_snapshot(service);
  if (service->env) HeapFree(GetProcessHeap(), 0, service->env);
  if (service->env_extra) HeapFree(GetProcessHeap(), 0, service->env_extra);
  if (service->process_handle) CloseHandle(service->process_handle);
  if (service->job_handle) CloseHandle(service->job_handle);
  if (service->stop_event) CloseHandle(service->stop_event);
  if (service->service_done_event) CloseHandle(service->service_done_event);
  if (service->lifecycle_section_initialised) {
    DeleteCriticalSection(&service->lifecycle_section);
  }
  if (service->throttle_section_initialised) {
    DeleteCriticalSection(&service->throttle_section);
  }
  if (service->health_section_initialised) {
    DeleteCriticalSection(&service->health_section);
  }
  if (service->resource_section_initialised) {
    DeleteCriticalSection(&service->resource_section);
  }
  if (service->initial_env) HeapFree(GetProcessHeap(), 0, service->initial_env);
  delete service->config;
  delete service->recovery_state;
  HeapFree(GetProcessHeap(), 0, service);
}
