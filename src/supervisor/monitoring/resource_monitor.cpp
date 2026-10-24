#include "config.h"
#include "event.h"
#include "failure.h"
#include "job_object.h"
#include "messages.h"
#include "service.h"
#include "status_snapshot.h"

#include <string>

namespace {

constexpr unsigned long kResourceWatchdogExitCode = 0xE0010003UL;

void terminate_for_resource(service_runtime_t *service, const wchar_t *reason,
                            const std::wstring &detail) {
  log_event(EVENTLOG_ERROR_TYPE, SVP_EVENT_RESOURCE_EXCEEDED, service->name,
            reason, detail.c_str(), 0);
  failure_kind_t kind = failure_kind_t::internal;
  if (wcscmp(reason, L"memory limit") == 0) {
    kind = failure_kind_t::resource_memory;
  }
  else if (wcscmp(reason, L"CPU limit") == 0) {
    kind = failure_kind_t::resource_cpu;
  }
  else if (wcscmp(reason, L"process limit") == 0) {
    kind = failure_kind_t::resource_processes;
  }
  record_runtime_failure(service, kind, L"resource",
                         kResourceWatchdogExitCode, ERROR_SUCCESS, detail);
  unsigned long error = ERROR_SUCCESS;
  if (service->job_handle &&
      ! terminate_service_job(service->job_handle, kResourceWatchdogExitCode,
                              &error)) {
    log_event(EVENTLOG_ERROR_TYPE, SVP_EVENT_TERMINATE_JOB_FAILED,
              service->name, error_string(error), 0);
  }
}

unsigned long WINAPI resource_monitor_thread(void *argument) {
  auto *service = static_cast<service_runtime_t *>(argument);
  const resource_limit_config_t &config = service->config->resources;
  HANDLE stop_event = service->resource_stop_event;
  unsigned long processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (! processors) processors = 1;

  job_accounting_t previous = {};
  unsigned long error = ERROR_SUCCESS;
  if (! query_service_job(service->job_handle, &previous, &error)) {
    if (config.enabled()) {
      terminate_for_resource(service, L"monitor failure",
                             L"initial Job Object query failed with error " +
                                 std::to_wstring(error));
      return kResourceWatchdogExitCode;
    }
    return 0;
  }
  unsigned long long previous_tick = GetTickCount64();
  unsigned long query_failures = 0;

  while (true) {
    const unsigned long wait =
        WaitForSingleObject(stop_event, config.monitor_interval_ms);
    if (wait == WAIT_OBJECT_0) return 0;
    if (wait != WAIT_TIMEOUT) {
      const unsigned long wait_error = GetLastError();
      if (config.enabled()) {
        terminate_for_resource(
            service, L"monitor failure",
            L"waiting for the monitor stop event failed with error " +
                std::to_wstring(wait_error));
        return kResourceWatchdogExitCode;
      }
      return 0;
    }
    job_accounting_t current = {};
    unsigned long long now = GetTickCount64();
    if (! query_service_job(service->job_handle, &current, &error)) {
      if (++query_failures >= 3) {
        if (config.enabled()) {
          terminate_for_resource(service, L"monitor failure",
                                 L"Job Object query failed with error " +
                                     std::to_wstring(error));
          return kResourceWatchdogExitCode;
        }
        return 0;
      }
      continue;
    }
    query_failures = 0;

    /*
      Windows enforces ActiveProcessLimit at process creation, but concurrent
      launches can momentarily overshoot it.  Keep the hard OS limit and prune
      the newest excess members so TasksMax remains a reliable upper bound.
    */
    if (config.maximum_processes &&
        current.active_processes > config.maximum_processes) {
      unsigned long enforce_error = ERROR_SUCCESS;
      if (! enforce_service_job_process_limit(
              service->job_handle, config.maximum_processes, service->pid,
              &enforce_error)) {
        terminate_for_resource(
            service, L"process limit",
            L"unable to terminate excess processes (error " +
                std::to_wstring(enforce_error) + L")");
        return kResourceWatchdogExitCode;
      }
      if (! query_service_job(service->job_handle, &current, &error)) {
        if (++query_failures >= 3) {
          terminate_for_resource(service, L"monitor failure",
                                 L"re-querying the Job Object failed with error " +
                                     std::to_wstring(error));
          return kResourceWatchdogExitCode;
        }
        continue;
      }
      query_failures = 0;
    }

    if (config.memory_limit_bytes &&
        current.job_memory >= config.memory_limit_bytes) {
      terminate_for_resource(
          service, L"memory limit",
          std::to_wstring(current.job_memory) + L" bytes used; limit " +
              std::to_wstring(config.memory_limit_bytes) + L" bytes");
      return kResourceWatchdogExitCode;
    }

    unsigned long long elapsed_ms = now > previous_tick ? now - previous_tick : 0;
    unsigned long percent = elapsed_ms
                                ? calculate_job_cpu_percent(
                                      previous, current, elapsed_ms, processors)
                                : 0;
    bool cpu_exceeded = false;
    EnterCriticalSection(&service->resource_section);
    service->resource_cpu_percent = percent;
    service->resource_memory_bytes = current.job_memory;
    service->resource_processes = current.active_processes;
    if (config.maximum_cpu_percent && elapsed_ms) {
      if (percent > config.maximum_cpu_percent) {
        ++service->resource_cpu_violations;
        cpu_exceeded = service->resource_cpu_violations >=
                       config.cpu_violation_threshold;
      }
      else service->resource_cpu_violations = 0;
    }
    LeaveCriticalSection(&service->resource_section);
    publish_status_snapshot(service);
    if (cpu_exceeded) {
      terminate_for_resource(
          service, L"CPU limit",
          std::to_wstring(percent) + L"% used; limit " +
              std::to_wstring(config.maximum_cpu_percent) + L"%");
      return kResourceWatchdogExitCode;
    }
    previous = current;
    previous_tick = now;
  }
}

}  // namespace

bool start_resource_monitor(service_runtime_t *service) {
  if (! service || ! service->config) {
    return true;
  }
  EnterCriticalSection(&service->health_section);
  EnterCriticalSection(&service->resource_section);
  if (service->stopping) {
    SetLastError(ERROR_OPERATION_ABORTED);
    LeaveCriticalSection(&service->resource_section);
    LeaveCriticalSection(&service->health_section);
    return false;
  }
  service->resource_cpu_violations = 0;
  service->resource_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (! service->resource_stop_event) {
    LeaveCriticalSection(&service->resource_section);
    LeaveCriticalSection(&service->health_section);
    return false;
  }
  service->resource_thread = CreateThread(nullptr, 0, resource_monitor_thread,
                                          service, 0, nullptr);
  if (! service->resource_thread) {
    unsigned long error = GetLastError();
    CloseHandle(service->resource_stop_event);
    service->resource_stop_event = nullptr;
    LeaveCriticalSection(&service->resource_section);
    LeaveCriticalSection(&service->health_section);
    SetLastError(error);
    return false;
  }
  LeaveCriticalSection(&service->resource_section);
  LeaveCriticalSection(&service->health_section);
  return true;
}

void stop_resource_monitor(service_runtime_t *service) {
  if (! service || ! service->resource_section_initialised) return;
  EnterCriticalSection(&service->resource_section);
  HANDLE thread = service->resource_thread;
  HANDLE event = service->resource_stop_event;
  if (event) SetEvent(event);
  LeaveCriticalSection(&service->resource_section);
  if (thread && GetThreadId(thread) != GetCurrentThreadId()) {
    WaitForSingleObject(thread, INFINITE);
  }
  EnterCriticalSection(&service->resource_section);
  if (service->resource_thread == thread) {
    if (thread) CloseHandle(thread);
    if (event) CloseHandle(event);
    service->resource_thread = nullptr;
    service->resource_stop_event = nullptr;
  }
  LeaveCriticalSection(&service->resource_section);
}

void record_job_memory_limit_failure(service_runtime_t *service,
                                     unsigned long exit_code) {
  if (! service || ! service->config || ! service->job_handle ||
      ! service->config->resources.memory_limit_bytes) {
    return;
  }
  job_accounting_t accounting = {};
  unsigned long error = ERROR_SUCCESS;
  if (! query_service_job(service->job_handle, &accounting, &error)) {
    log_event(EVENTLOG_WARNING_TYPE, SVP_EVENT_RESOURCE_EXCEEDED,
              service->name, L"memory limit",
              (L"unable to inspect peak Job memory after process exit: error " +
               std::to_wstring(error)).c_str(),
              0);
    return;
  }
  const unsigned long long limit = service->config->resources.memory_limit_bytes;
  if (accounting.peak_job_memory < limit) return;
  const std::wstring detail =
      L"process exceeded the Job memory limit of " + std::to_wstring(limit) +
      L" bytes; peak was " + std::to_wstring(accounting.peak_job_memory) +
      L" bytes";
  if (record_runtime_failure(service, failure_kind_t::resource_memory,
                             L"resource", exit_code, ERROR_SUCCESS, detail)) {
    log_event(EVENTLOG_ERROR_TYPE, SVP_EVENT_RESOURCE_EXCEEDED,
              service->name, L"memory limit", detail.c_str(), 0);
  }
}
