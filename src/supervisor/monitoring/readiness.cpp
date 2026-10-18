#include "readiness.h"

#include "health_check.h"

#include <algorithm>
#include <utility>

namespace {

readiness_result_t process_exited(HANDLE process) {
  health_check_config_t probe;
  probe.type = health_check_type_t::process;
  health_probe_result_t result = run_health_probe(probe, process, nullptr,
                                                  nullptr);
  return {readiness_state_t::process_exited,
          result.error ? result.error : ERROR_PROCESS_ABORTED,
          std::move(result.detail)};
}

void report_start_pending(SERVICE_STATUS_HANDLE status_handle,
                          SERVICE_STATUS *status, unsigned long wait_hint,
                          SRWLOCK *status_lock) {
  if (! status_handle || ! status) return;
  if (status_lock) AcquireSRWLockExclusive(status_lock);
  status->dwCurrentState = SERVICE_START_PENDING;
  status->dwWaitHint = std::max<unsigned long>(wait_hint, 1000);
  ++status->dwCheckPoint;
  SetServiceStatus(status_handle, status);
  if (status_lock) ReleaseSRWLockExclusive(status_lock);
}

bool wait_interval(HANDLE process, HANDLE cancellation_event,
                   unsigned long milliseconds,
                   readiness_result_t *failure) {
  HANDLE handles[] = {process, cancellation_event};
  unsigned long result = cancellation_event
                             ? WaitForMultipleObjects(2, handles, FALSE,
                                                      milliseconds)
                             : WaitForSingleObject(process, milliseconds);
  if (result == WAIT_TIMEOUT) return true;
  if (result == WAIT_OBJECT_0) {
    *failure = process_exited(process);
  }
  else if (cancellation_event && result == WAIT_OBJECT_0 + 1) {
    *failure = {readiness_state_t::cancelled, ERROR_CANCELLED,
                L"service stop was requested"};
  }
  else {
    *failure = {readiness_state_t::error, GetLastError(),
                L"waiting for the application process failed"};
  }
  return false;
}

}  // namespace

readiness_result_t wait_for_readiness(
    const readiness_config_t &config, HANDLE application_process,
    const wchar_t *environment, const wchar_t *working_directory,
    HANDLE cancellation_event,
    SERVICE_STATUS_HANDLE status_handle, SERVICE_STATUS *service_status,
    SRWLOCK *status_lock) {
  if (! config.enabled()) {
    return {readiness_state_t::ready, ERROR_SUCCESS,
            L"readiness probe is disabled"};
  }
  if (! application_process) {
    return {readiness_state_t::error, ERROR_INVALID_HANDLE,
            L"application process handle is unavailable"};
  }

  const unsigned long long started = GetTickCount64();
  const unsigned long long deadline = started + config.startup_timeout_ms;
  if (config.probe.initial_delay_ms) {
    report_start_pending(status_handle, service_status,
                         config.probe.initial_delay_ms, status_lock);
    readiness_result_t failure;
    if (! wait_interval(application_process, cancellation_event,
                        config.probe.initial_delay_ms, &failure)) {
      return failure;
    }
  }

  unsigned long consecutive_successes = 0;
  std::wstring last_detail = L"readiness probe did not complete";
  while (true) {
    if (cancellation_event &&
        WaitForSingleObject(cancellation_event, 0) == WAIT_OBJECT_0) {
      return {readiness_state_t::cancelled, ERROR_CANCELLED,
              L"service stop was requested"};
    }
    unsigned long long now = GetTickCount64();
    if (now >= deadline) {
      return {readiness_state_t::timed_out, ERROR_TIMEOUT,
              L"startup timeout expired: " + last_detail};
    }
    unsigned long remaining = static_cast<unsigned long>(deadline - now);
    unsigned long wait_hint = std::min<unsigned long>(
        remaining, config.probe.timeout_ms + config.probe.interval_ms + 1000);
    report_start_pending(status_handle, service_status, wait_hint,
                         status_lock);

    health_check_config_t probe = config.probe;
    probe.timeout_ms = std::min(probe.timeout_ms, remaining);
    health_probe_result_t result = run_health_probe(
        probe, application_process, environment, working_directory);
    last_detail = result.detail;
    if (! result.healthy && result.error != ERROR_SUCCESS) {
      last_detail.append(L" (error ");
      last_detail.append(std::to_wstring(result.error));
      last_detail.push_back(L')');
    }

    if (result.healthy) {
      ++consecutive_successes;
      if (consecutive_successes >= config.probe.success_threshold) {
        return {readiness_state_t::ready, ERROR_SUCCESS, std::move(last_detail)};
      }
    }
    else {
      consecutive_successes = 0;
    }

    now = GetTickCount64();
    if (now >= deadline) {
      return {readiness_state_t::timed_out, ERROR_TIMEOUT,
              L"startup timeout expired: " + last_detail};
    }
    remaining = static_cast<unsigned long>(deadline - now);
    readiness_result_t failure;
    if (! wait_interval(application_process, cancellation_event,
                        std::min(config.probe.interval_ms, remaining),
                        &failure)) {
      return failure;
    }
  }
}
