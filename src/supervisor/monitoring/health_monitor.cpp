#include "nssm.h"
#include "child_process.h"
#include "config.h"
#include "health_check.h"
#include "job_object.h"
#include "event.h"
#include "messages.h"
#include "service.h"
#include "status_snapshot.h"

#include <string>
#include <vector>

namespace {

constexpr unsigned long kHealthWatchdogExitCode = 0xE0010001UL;

unsigned long fail_health_monitor(nssm_service_t *service,
                                  unsigned long error,
                                  const wchar_t *operation) {
  std::wstring detail = std::wstring(operation) + L" failed with error " +
                        std::to_wstring(error);
  log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_HEALTH_WATCHDOG,
            service->name, detail.c_str(), 0);
  unsigned long termination_error = ERROR_SUCCESS;
  if (service->job_handle &&
      ! terminate_service_job(service->job_handle, kHealthWatchdogExitCode,
                              &termination_error)) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_TERMINATE_JOB_FAILED,
              service->name, error_string(termination_error), 0);
  }
  return kHealthWatchdogExitCode;
}

unsigned long WINAPI health_monitor_thread(void *argument) {
  auto *service = static_cast<nssm_service_t *>(argument);
  const health_check_config_t &config = service->config->health;
  HANDLE stop_event = service->health_stop_event;

  std::vector<wchar_t> environment;
  std::wstring environment_error;
  bool environment_ready =
      config.type != health_check_type_t::command ||
      build_child_environment(service->initial_env, service->env,
                              service->env_extra, &environment,
                              &environment_error);

  const unsigned long initial_wait =
      WaitForSingleObject(stop_event, config.initial_delay_ms);
  if (initial_wait == WAIT_OBJECT_0) return 0;
  if (initial_wait != WAIT_TIMEOUT) {
    return fail_health_monitor(service, GetLastError(), L"initial wait");
  }

  while (true) {
    health_probe_result_t result;
    if (! environment_ready) {
      result = {false, ERROR_INVALID_ENVIRONMENT,
                L"building command probe environment failed: " +
                    environment_error};
    }
    else {
      result = run_health_probe(
          config, service->process_handle,
          environment.empty() ? nullptr : environment.data(), service->dir);
    }
    std::wstring detail = result.detail;
    if (! result.healthy && result.error != ERROR_SUCCESS) {
      detail.append(L" (error ");
      detail.append(std::to_wstring(result.error));
      detail.push_back(L')');
    }

    bool recovered = false;
    bool first_failure = false;
    bool watchdog = false;
    unsigned long failure_count = 0;
    EnterCriticalSection(&service->health_section);
    if (result.healthy) {
      service->health_failures = 0;
      ++service->health_successes;
      if (service->health_degraded &&
          service->health_successes >= config.success_threshold) {
        service->health_degraded = false;
        recovered = true;
      }
    }
    else {
      service->health_successes = 0;
      ++service->health_failures;
      if (! service->health_degraded) {
        service->health_degraded = true;
        first_failure = true;
      }
      failure_count = service->health_failures;
      watchdog = failure_count >= config.failure_threshold;
    }
    LeaveCriticalSection(&service->health_section);

    if (recovered) {
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_HEALTH_RECOVERED,
                service->name, detail.c_str(), 0);
    }
    if (first_failure) {
      TCHAR count[32] = {};
      _sntprintf_s(count, _countof(count), _TRUNCATE, _T("%lu"),
                   failure_count);
      log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_HEALTH_FAILED,
                service->name, detail.c_str(), count, 0);
    }
    publish_status_snapshot(service);
    if (watchdog) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_HEALTH_WATCHDOG,
                service->name, detail.c_str(), 0);
      unsigned long error = ERROR_SUCCESS;
      if (service->job_handle &&
          ! terminate_service_job(service->job_handle,
                                  kHealthWatchdogExitCode, &error)) {
        log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_TERMINATE_JOB_FAILED,
                  service->name, error_string(error), 0);
      }
      return kHealthWatchdogExitCode;
    }

    const unsigned long interval_wait =
        WaitForSingleObject(stop_event, config.interval_ms);
    if (interval_wait == WAIT_OBJECT_0) return 0;
    if (interval_wait != WAIT_TIMEOUT) {
      return fail_health_monitor(service, GetLastError(), L"interval wait");
    }
  }
}

}  // namespace

bool start_health_monitor(nssm_service_t *service) {
  if (! service || ! service->config ||
      service->config->health.type == health_check_type_t::disabled) {
    return true;
  }
  EnterCriticalSection(&service->health_section);
  if (service->stopping) {
    SetLastError(ERROR_OPERATION_ABORTED);
    LeaveCriticalSection(&service->health_section);
    return false;
  }
  service->health_failures = 0;
  service->health_successes = 0;
  service->health_degraded = false;
  service->health_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (! service->health_stop_event) {
    LeaveCriticalSection(&service->health_section);
    return false;
  }
  service->health_thread = CreateThread(nullptr, 0, health_monitor_thread,
                                        service, 0, nullptr);
  if (! service->health_thread) {
    unsigned long error = GetLastError();
    CloseHandle(service->health_stop_event);
    service->health_stop_event = nullptr;
    LeaveCriticalSection(&service->health_section);
    SetLastError(error);
    return false;
  }
  LeaveCriticalSection(&service->health_section);
  return true;
}

void stop_health_monitor(nssm_service_t *service) {
  if (! service || ! service->health_section_initialised) return;
  EnterCriticalSection(&service->health_section);
  HANDLE thread = service->health_thread;
  HANDLE event = service->health_stop_event;
  if (event) SetEvent(event);
  LeaveCriticalSection(&service->health_section);
  if (thread && GetThreadId(thread) != GetCurrentThreadId()) {
    WaitForSingleObject(thread, INFINITE);
  }
  EnterCriticalSection(&service->health_section);
  if (service->health_thread == thread) {
    if (thread) CloseHandle(thread);
    if (event) CloseHandle(event);
    service->health_thread = nullptr;
    service->health_stop_event = nullptr;
  }
  LeaveCriticalSection(&service->health_section);
}
