#include "service.h"

#include "config.h"
#include "event.h"
#include "health_monitor.h"
#include "messages.h"
#include "nssm_io.h"
#include "notification.h"
#include "resource_monitor.h"
#include "recovery.h"
#include "service_constants.h"
#include "service_process_wait.h"
#include "status_snapshot.h"

void set_nssm_service_defaults(nssm_service_t *service) {
  if (! service) return;

  InitializeSRWLock(&service->status_snapshot_lock);
  InitializeSRWLock(&service->status_lock);
  InitializeSRWLock(&service->wait_lock);
  service->stdin_sharing = NSSM_STDIN_SHARING;
  service->stdin_disposition = NSSM_STDIN_DISPOSITION;
  service->stdin_flags = NSSM_STDIN_FLAGS;
  service->stdout_sharing = NSSM_STDOUT_SHARING;
  service->stdout_disposition = NSSM_STDOUT_DISPOSITION;
  service->stdout_flags = NSSM_STDOUT_FLAGS;
  service->stderr_sharing = NSSM_STDERR_SHARING;
  service->stderr_disposition = NSSM_STDERR_DISPOSITION;
  service->stderr_flags = NSSM_STDERR_FLAGS;
  service->kill_console_delay = kKillConsoleGracePeriodMs;
  service->kill_window_delay = kKillWindowGracePeriodMs;
  service->kill_threads_delay = kKillThreadsGracePeriodMs;
  service->kill_process_tree = true;
}

nssm_service_t *alloc_nssm_service() {
  auto *service = static_cast<nssm_service_t *>(
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(nssm_service_t)));
  if (! service) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"service",
              L"alloc_nssm_service()", 0);
  }
  return service;
}

void cleanup_nssm_service(nssm_service_t *service) {
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
