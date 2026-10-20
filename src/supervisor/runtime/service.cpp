#include "child_process.h"
#include "command_runner.h"
#include "config.h"
#include "config_registration.h"
#include "env.h"
#include "event.h"
#include "job_object.h"
#include "health_monitor.h"
#include "messages.h"
#include "nssm_io.h"
#include "notification.h"
#include "process.h"
#include "readiness.h"
#include "security.h"
#include "service.h"
#include "service_config.h"
#include "service_constants.h"
#include "service_control.h"
#include "service_process_wait.h"
#include "service_status.h"
#include "service_wait.h"
#include "status_snapshot.h"
#include "recovery.h"
#include "resource_monitor.h"
#include "win32_handle.h"

#include <algorithm>
#include <string>
#include <vector>

static constexpr int kStartHandledByRecovery = -1;
static constexpr int kStartAwaitingProcessExit = -2;

static void set_service_recovery(nssm_service_t *, SC_HANDLE);

static unsigned long remaining_until(unsigned long long deadline) {
  const unsigned long long now = GetTickCount64();
  if (now >= deadline) return 0;
  const unsigned long long remaining = deadline - now;
  return remaining > MAXDWORD ? MAXDWORD
                              : static_cast<unsigned long>(remaining);
}

class critical_section_guard {
 public:
  explicit critical_section_guard(CRITICAL_SECTION *section)
      : section_(section) {
    EnterCriticalSection(section_);
  }
  ~critical_section_guard() { LeaveCriticalSection(section_); }
  critical_section_guard(const critical_section_guard &) = delete;
  critical_section_guard &operator=(const critical_section_guard &) = delete;

 private:
  CRITICAL_SECTION *section_;
};

template <typename Mutator>
void update_service_status(nssm_service_t *service, Mutator mutator) {
  AcquireSRWLockExclusive(&service->status_lock);
  mutator(service->status);
  report_service_status(service->status_handle, &service->status,
                        service->name, L"updating service state");
  ReleaseSRWLockExclusive(&service->status_lock);
}

static bool run_lifecycle_commands(
    nssm_service_t *service, const std::vector<command_config_t> &commands,
    const wchar_t *phase, wchar_t *environment, HANDLE cancellation_event,
    unsigned long long deadline, bool failure_is_fatal) {
  for (size_t index = 0; index < commands.size(); ++index) {
    const unsigned long remaining = remaining_until(deadline);
    if (! remaining) {
      log_event(failure_is_fatal ? EVENTLOG_ERROR_TYPE : EVENTLOG_WARNING_TYPE,
                NSSM_EVENT_LIFECYCLE_COMMAND_FAILED, service->name, phase,
                L"lifecycle deadline expired", 0);
      return false;
    }
    update_service_status(service, [remaining](SERVICE_STATUS &status) {
      ++status.dwCheckPoint;
      status.dwWaitHint = remaining + kServiceWaitHintMarginMs;
    });
    command_result_t result = run_command(
        commands[index], environment, service->dir, cancellation_event,
        remaining);
    if (! result.succeeded()) {
      std::wstring detail = L"command " + std::to_wstring(index + 1) +
                            L": " + result.detail;
      log_event(failure_is_fatal ? EVENTLOG_ERROR_TYPE : EVENTLOG_WARNING_TYPE,
                NSSM_EVENT_LIFECYCLE_COMMAND_FAILED, service->name, phase,
                detail.c_str(), 0);
      return false;
    }
  }
  return true;
}

static bool start_detached_thread(LPTHREAD_START_ROUTINE function,
                                  void *context) {
  unique_handle thread(CreateThread(nullptr, 0, function, context, 0,
                                    nullptr));
  if (! thread) return false;
  return true;
}

static void terminate_after_monitor_start_failure(nssm_service_t *service) {
  EnterCriticalSection(&service->health_section);
  if (! service->stopping && service->job_handle) {
    terminate_service_job(service->job_handle, ERROR_NOT_READY, nullptr);
  }
  LeaveCriticalSection(&service->health_section);
}

static bool restart_allowed(nssm_service_t *service) {
  EnterCriticalSection(&service->health_section);
  const bool allowed = service->allow_restart;
  LeaveCriticalSection(&service->health_section);
  return allowed;
}

static void set_restart_allowed(nssm_service_t *service, bool allowed) {
  EnterCriticalSection(&service->health_section);
  service->allow_restart = allowed;
  LeaveCriticalSection(&service->health_section);
}

static void reset_throttle(nssm_service_t *service, bool wake_waiter) {
  EnterCriticalSection(&service->throttle_section);
  service->throttle = 0;
  if (wake_waiter) WakeConditionVariable(&service->throttle_condition);
  LeaveCriticalSection(&service->throttle_section);
}

static bool prepare_child_launch(nssm_service_t *service,
                                 std::wstring *command_line,
                                 std::vector<wchar_t> *environment) {
  std::wstring error;
  if (! build_child_command_line(service->exe, service->flags, command_line,
                                 &error) ||
      ! build_child_environment(service->initial_env, service->env,
                                service->env_extra, environment, &error)) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEPROCESS_FAILED,
              service->name, service->exe, error.c_str(), 0);
    return false;
  }
  return true;
}

static int finish_service_start(nssm_service_t *service,
                                wchar_t *environment,
                                unsigned long long deadline) {
  if (service->config->lifecycle.notify) {
    const unsigned long remaining = remaining_until(deadline);
    std::wstring detail;
    if (! remaining ||
        ! wait_for_ready_notification(service, remaining, &detail)) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_READINESS_FAILED,
                service->name, detail.c_str(), 0);
      if (service->job_handle) {
        terminate_service_job(service->job_handle, ERROR_NOT_READY, nullptr);
      }
      return kStartAwaitingProcessExit;
    }
    reset_throttle(service, false);
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_READINESS_READY,
              service->name, detail.c_str(), 0);
  }
  else if (service->config->readiness.enabled()) {
    readiness_config_t readiness_config = service->config->readiness;
    const unsigned long remaining = remaining_until(deadline);
    if (! remaining) return stop_service(service, 3);
    readiness_config.startup_timeout_ms =
        std::min(readiness_config.startup_timeout_ms, remaining);
    readiness_result_t readiness = wait_for_readiness(
        readiness_config, service->process_handle, environment,
        service->dir, service->stop_event, service->status_handle,
        &service->status, &service->status_lock);
    if (! readiness.ready()) {
      if (readiness.state == readiness_state_t::cancelled) {
        return kStartHandledByRecovery;
      }
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_READINESS_FAILED,
                service->name, readiness.detail.c_str(), 0);
      if (readiness.state != readiness_state_t::process_exited &&
          service->job_handle) {
        terminate_service_job(service->job_handle, ERROR_NOT_READY, nullptr);
        WaitForSingleObject(service->process_handle,
                            kServiceStatusDeadlineMs);
      }
      return kStartAwaitingProcessExit;
    }
    reset_throttle(service, false);
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_READINESS_READY,
              service->name, readiness.detail.c_str(), 0);
  }
  else {
    const int stability = await_single_handle(
        service->status_handle, &service->status, service->process_handle,
        service->name, L"start_service", kResetThrottleRestartMs,
        &service->status_lock);
    if (stability == 0) {
      return kStartAwaitingProcessExit;
    }
    if (stability < 0) {
      if (WaitForSingleObject(service->stop_event, 0) == WAIT_OBJECT_0) {
        return kStartHandledByRecovery;
      }
      return stop_service(service, 3);
    }
    reset_throttle(service, false);
  }

  if (! run_lifecycle_commands(
          service, service->config->lifecycle.exec_start_post,
          L"ExecStartPost", environment, service->stop_event, deadline, true)) {
    if (service->job_handle) {
      terminate_service_job(service->job_handle, ERROR_PROCESS_ABORTED,
                            nullptr);
      WaitForSingleObject(service->process_handle, kServiceStatusDeadlineMs);
    }
    return kStartAwaitingProcessExit;
  }

  if (! restart_allowed(service)) return 0;

  update_service_status(service, [](SERVICE_STATUS &status) {
    status.dwCurrentState = SERVICE_RUNNING;
    status.dwCheckPoint = 0;
    status.dwWaitHint = 0;
    status.dwControlsAccepted &= ~SERVICE_ACCEPT_PAUSE_CONTINUE;
  });
  publish_status_snapshot(service);
  return 0;
}

static void close_failed_child_launch(nssm_service_t *service,
                                      HANDLE thread_handle) {
  if (thread_handle) CloseHandle(thread_handle);
  if (service->process_handle) CloseHandle(service->process_handle);
  if (service->job_handle) CloseHandle(service->job_handle);
  service->process_handle = nullptr;
  service->job_handle = nullptr;
  service->pid = 0;
}

static bool configure_service_job(nssm_service_t *service) {
  unsigned long error = ERROR_SUCCESS;
  service->job_handle = create_service_job(&error);
  if (! service->job_handle) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATE_JOB_FAILED,
              service->name, error_string(error), 0);
    return false;
  }
  if (service->config->resources.enabled() &&
      ! configure_service_job_limits(
          service->job_handle,
          service->config->resources.maximum_processes,
          service->config->resources.memory_limit_bytes, &error)) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CONFIGURE_JOB_FAILED,
              service->name, error_string(error), 0);
    CloseHandle(service->job_handle);
    service->job_handle = nullptr;
    return false;
  }
  return true;
}

static bool create_suspended_service_process(
    nssm_service_t *service, STARTUPINFOW *startup_info,
    std::wstring *command_line, wchar_t *environment,
    PROCESS_INFORMATION *process_info) {
  const bool inherit_handles =
      (startup_info->dwFlags & STARTF_USESTDHANDLES) != 0;
  unsigned long flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
  if (! service->no_console) flags |= CREATE_NEW_CONSOLE;

  if (! CreateProcessW(service->exe, command_line->data(), nullptr, nullptr,
                      inherit_handles, flags, environment, service->dir,
                      startup_info, process_info)) {
    const unsigned long error = GetLastError();
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEPROCESS_FAILED,
              service->name, service->exe, error_string(error), 0);
    return false;
  }
  service->process_handle = process_info->hProcess;
  service->pid = process_info->dwProcessId;
  service->process_started_ms = GetTickCount64();
  publish_status_snapshot(service);
  return true;
}

static bool assign_service_process_to_job(nssm_service_t *service,
                                          HANDLE thread_handle) {
  unsigned long error = ERROR_SUCCESS;
  if (assign_process_to_service_job(service->job_handle,
                                    service->process_handle, &error)) {
    return true;
  }
  log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_ASSIGN_JOB_FAILED, service->name,
            error_string(error), 0);
  TerminateProcess(service->process_handle, error);
  WaitForSingleObject(service->process_handle, kServiceStatusDeadlineMs);
  close_failed_child_launch(service, thread_handle);
  return false;
}

static bool resume_service_process(nssm_service_t *service,
                                   HANDLE thread_handle) {
  if (ResumeThread(thread_handle) != static_cast<unsigned long>(-1)) {
    CloseHandle(thread_handle);
    return true;
  }

  const unsigned long error = GetLastError();
  log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_RESUME_PROCESS_FAILED,
            service->name, error_string(error), 0);
  terminate_service_job(service->job_handle, error, nullptr);
  WaitForSingleObject(service->process_handle, kServiceStatusDeadlineMs);
  close_failed_child_launch(service, thread_handle);
  return false;
}

static inline unsigned long throttle_milliseconds(unsigned long throttle) {
  if (throttle > 7) throttle = 8;
  /* pow() operates on doubles. */
  unsigned long ret = 1; for (unsigned long i = 1; i < throttle; i++) ret *= 2;
  return ret * 1000;
}

/*
  Wrapper to be called in a new thread so that we can acknowledge a STOP
  control immediately.
*/
static unsigned long WINAPI shutdown_service(void *arg) {
  return stop_service((nssm_service_t *) arg, 0);
}

/*
 Wrapper to be called in a new thread so that we can acknowledge start
 immediately.
*/
static unsigned long WINAPI launch_service(void *arg) {
  return monitor_service((nssm_service_t *) arg);
}

static unsigned long WINAPI reload_service(void *arg) {
  auto *service = static_cast<nssm_service_t *>(arg);
  critical_section_guard lifecycle_guard(&service->lifecycle_section);
  if (! service->config || service->config->lifecycle.exec_reload.empty() ||
      ! service->process_handle ||
      WaitForSingleObject(service->stop_event, 0) == WAIT_OBJECT_0) {
    return ERROR_CALL_NOT_IMPLEMENTED;
  }
  std::vector<wchar_t> environment;
  std::wstring error;
  if (! build_child_environment(service->initial_env, service->env,
                                service->env_extra, &environment, &error)) {
    return ERROR_INVALID_DATA;
  }
  const unsigned long long deadline =
      GetTickCount64() + service->config->lifecycle.timeout_start_ms;
  return run_lifecycle_commands(service,
                                service->config->lifecycle.exec_reload,
                                L"ExecReload", environment.data(),
                                service->stop_event, deadline, true)
             ? ERROR_SUCCESS
             : ERROR_PROCESS_ABORTED;
}

/* About to install the service */
/* Service initialisation */
void WINAPI service_main(unsigned long argc, wchar_t **argv) {
  nssm_service_t *service = alloc_nssm_service();
  if (! service) return;
  set_nssm_service_defaults(service);

  (void) argc;

  if (_snwprintf_s(service->name, _countof(service->name), _TRUNCATE, L"%ls", argv[0]) < 0) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, L"service->name", L"service_main()", 0);
    cleanup_nssm_service(service);
    return;
  }

  /* Initialise status */
  ZeroMemory(&service->status, sizeof(service->status));
  /*
    On modern Windows, interactive services are effectively unsupported due to
    Session 0 isolation. Report our base service type consistently.
  */
  service->status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service->status.dwControlsAccepted = 0;
  service->status.dwWin32ExitCode = NO_ERROR;
  service->status.dwServiceSpecificExitCode = 0;
  service->status.dwCheckPoint = 0;
  service->status.dwWaitHint = kServiceWaitHintMarginMs;

  /* Signal we AREN'T running the server */
  service->process_handle = 0;
  service->pid = 0;

  /* Initialise all state which can be reached by the control handler. */
  InitializeCriticalSection(&service->throttle_section);
  InitializeConditionVariable(&service->throttle_condition);
  service->throttle_section_initialised = true;
  InitializeCriticalSection(&service->lifecycle_section);
  service->lifecycle_section_initialised = true;
  InitializeCriticalSection(&service->health_section);
  service->health_section_initialised = true;
  InitializeCriticalSection(&service->resource_section);
  service->resource_section_initialised = true;
  service->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  service->service_done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (! service->stop_event || ! service->service_done_event) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEEVENT_FAILED,
              service->name, error_string(GetLastError()), 0);
    cleanup_nssm_service(service);
    return;
  }
  service->allow_restart = true;

  /* Register control handler */
  service->status_handle = RegisterServiceCtrlHandlerExW(
      kProductName, service_control_handler, service);
  if (! service->status_handle) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_REGISTERSERVICECTRLHANDER_FAILED, error_string(GetLastError()), 0);
    cleanup_nssm_service(service);
    return;
  }

  log_service_control(service->name, 0, true);

  update_service_status(service, [](SERVICE_STATUS &status) {
    status.dwCurrentState = SERVICE_START_PENDING;
    status.dwWaitHint =
        kResetThrottleRestartMs + kServiceWaitHintMarginMs;
  });

  if (is_process_administrator()) {
    unique_service_handle manager(OpenSCManagerW(
        nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_CONNECT));
    if (manager) {
      unique_service_handle own_service(OpenServiceW(
          manager.get(), service->name, SERVICE_CHANGE_CONFIG));
      if (own_service) set_service_recovery(service, own_service.get());
    }
  }

  /* Remember our initial environment. */
  service->initial_env = copy_environment();
  if (! service->initial_env) {
    const unsigned long error = GetLastError();
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CONFIG_LOAD_FAILED,
              service->name, error_string(error), 0);
    stop_service(service, 2);
    cleanup_nssm_service(service);
    return;
  }

  config_validation_t config_validation;
  if (! load_registered_service_config(service, &config_validation)) {
    std::wstring details;
    for (const auto &error : config_validation.errors) {
      if (! details.empty()) details.append(L"; ");
      details.append(error);
    }
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CONFIG_LOAD_FAILED, service->name,
              details.c_str(), 0);
    stop_service(service, 2);
    cleanup_nssm_service(service);
    return;
  }
  if (! create_status_snapshot(service)) {
    log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_STATUS_SNAPSHOT_FAILED,
              service->name, error_string(GetLastError()), 0);
  }

  /* Remember our creation time. */
  if (get_process_creation_time(GetCurrentProcess(), &service->nssm_creation_time)) ZeroMemory(&service->nssm_creation_time, sizeof(service->nssm_creation_time));

  if (WaitForSingleObject(service->stop_event, 0) == WAIT_OBJECT_0) return;
  if (! start_detached_thread(launch_service, service)) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED, error_string(GetLastError()), 0);
    stop_service(service, 0);
    cleanup_nssm_service(service);
    return;
  }

  /*
    A service's ServiceMain function must remain alive for the complete
    lifetime reported to the SCM.  Returning here left only detached worker
    threads behind, allowing the dispatcher to tear down the service process
    during an asynchronous stop before ExecStopPost and SERVICE_STOPPED were
    reported.
  */
  WaitForSingleObject(service->service_done_event, INFINITE);
}

/* Make sure service recovery actions are taken where necessary */
static void set_service_recovery(nssm_service_t *service,
                                 SC_HANDLE service_handle) {
  SERVICE_FAILURE_ACTIONS_FLAG flag;
  ZeroMemory(&flag, sizeof(flag));
  flag.fFailureActionsOnNonCrashFailures = true;

  if (! ChangeServiceConfig2W(service_handle,
                             SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag)) {
    unsigned long error = GetLastError();
    log_event(EVENTLOG_ERROR_TYPE,
              NSSM_EVENT_SERVICE_CONFIG_FAILURE_ACTIONS_FAILED,
              service->name, error_string(error), 0);
  }
}

int monitor_service(nssm_service_t *service) {
  /* Set service status to started */
  int ret = start_service(service);
  if (ret == kStartHandledByRecovery) return 0;
  const bool awaiting_process_exit = ret == kStartAwaitingProcessExit;
  if (ret && ! awaiting_process_exit) {
    wchar_t code[16];
    _snwprintf_s(code, _countof(code), _TRUNCATE, L"%d", ret);
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_START_SERVICE_FAILED,
              service->exe, service->name, code, 0);
    return ret;
  }
  if (! service->process_handle ||
      WaitForSingleObject(service->stop_event, 0) == WAIT_OBJECT_0) {
    return 0;
  }
  /* Monitor service */
  if (! register_service_process_wait(service)) {
    const unsigned long error = GetLastError();
    if (error == ERROR_OPERATION_ABORTED) return 0;
    log_event(EVENTLOG_ERROR_TYPE,
              NSSM_EVENT_REGISTERWAITFORSINGLEOBJECT_FAILED, service->name,
              service->exe, error_string(error), 0);
    return stop_service(service, 3);
  }
  if (awaiting_process_exit) return 0;

  log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_STARTED_SERVICE,
            service->exe, service->flags, service->name, service->dir, 0);
  if (! start_notification_watchdog(service)) {
    terminate_after_monitor_start_failure(service);
    return ERROR_NOT_READY;
  }
  if (! start_health_monitor(service)) {
    const unsigned long error = GetLastError();
    if (error == ERROR_OPERATION_ABORTED) return 0;
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_HEALTH_MONITOR_START_FAILED,
              service->name, error_string(error), 0);
    terminate_after_monitor_start_failure(service);
  }
  else if (! start_resource_monitor(service)) {
    const unsigned long error = GetLastError();
    if (error == ERROR_OPERATION_ABORTED) return 0;
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_RESOURCE_MONITOR_START_FAILED,
              service->name, error_string(error), 0);
    terminate_after_monitor_start_failure(service);
  }

  return 0;
}

/* Service control handler */
unsigned long WINAPI service_control_handler(unsigned long control, unsigned long event, void *data, void *context) {
  (void) event;
  (void) data;
  nssm_service_t *service = (nssm_service_t *) context;

  switch (control) {
    case SERVICE_CONTROL_INTERROGATE:
      /* We always keep the service status up-to-date so this is a no-op. */
      return NO_ERROR;

    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_STOP:
      log_service_control(service->name, control, true);

      /* Immediately block further controls. */
      set_restart_allowed(service, false);
      WakeConditionVariable(&service->throttle_condition);
      SetEvent(service->stop_event);
      update_service_status(service, [](SERVICE_STATUS &status) {
        status.dwCurrentState = SERVICE_STOP_PENDING;
        status.dwControlsAccepted = 0;
      });

      /*
        We MUST acknowledge the stop request promptly but we're committed to
        waiting for the application to exit.  Spawn a new thread to wait
        while we acknowledge the request.
      */
      if (! start_detached_thread(shutdown_service, context)) {
        log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED, error_string(GetLastError()), 0);

        /*
          We couldn't create a thread to tidy up so we'll have to force the tidyup
          to complete in time in this thread.
        */
        service->kill_console_delay = kKillConsoleGracePeriodMs;
        service->kill_window_delay = kKillWindowGracePeriodMs;
        service->kill_threads_delay = kKillThreadsGracePeriodMs;

        stop_service(service, 0);
      }
      return NO_ERROR;

    case SERVICE_CONTROL_CONTINUE: {
      log_service_control(service->name, control, true);
      reset_throttle(service, true);
      const unsigned long wait_hint =
          throttle_milliseconds(0) + kServiceWaitHintMarginMs;
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_RESET_THROTTLE, service->name, 0);
      update_service_status(service, [wait_hint](SERVICE_STATUS &status) {
        if (status.dwCurrentState != SERVICE_RUNNING) {
          status.dwCurrentState = SERVICE_CONTINUE_PENDING;
        }
        status.dwWaitHint = wait_hint;
      });
      return NO_ERROR;
    }

    case SERVICE_CONTROL_PAUSE:
      /*
        We don't accept pause messages but it isn't possible to register
        only for continue messages so we have to handle this case.
      */
      log_service_control(service->name, control, false);
      return ERROR_CALL_NOT_IMPLEMENTED;

    case kServiceControlRotate:
      log_service_control(service->name, control, true);
      request_logger_rotation(&service->rotate_stdout_online);
      request_logger_rotation(&service->rotate_stderr_online);
      return NO_ERROR;

    case kServiceControlReload:
      log_service_control(service->name, control, true);
      if (! service->config ||
          service->config->lifecycle.exec_reload.empty()) {
        return ERROR_CALL_NOT_IMPLEMENTED;
      }
      if (! start_detached_thread(reload_service, service)) {
        return GetLastError();
      }
      return NO_ERROR;
  }

  /* Unknown control */
  log_service_control(service->name, control, false);
  return ERROR_CALL_NOT_IMPLEMENTED;
}

/* Start the service */
int start_service(nssm_service_t *service) {
  critical_section_guard lifecycle_guard(&service->lifecycle_section);
  service->stop_post_ran = false;
  if (service->health_section_initialised) {
    EnterCriticalSection(&service->health_section);
    service->stopping = false;
    LeaveCriticalSection(&service->health_section);
  }
  else service->stopping = false;

  if (service->process_handle) return 0;
  const unsigned long long start_deadline =
      GetTickCount64() + service->config->lifecycle.timeout_start_ms;
  /* Allocate a STARTUPINFO structure for a new process */
  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);

  /* Allocate a PROCESSINFO structure for the process */
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  std::wstring command_line;
  std::vector<wchar_t> child_environment;
  if (! prepare_child_launch(service, &command_line, &child_environment)) {
    return stop_service(service, 2);
  }

  if (service->config->lifecycle.notify) {
    std::wstring endpoint;
    if (! start_notification_listener(service, &endpoint)) {
      return stop_service(service, 3);
    }
    std::vector<environment_entry_t> notification_environment = {
        {L"SVP_NOTIFY_PIPE", endpoint}};
    std::vector<wchar_t> extended;
    std::wstring environment_error;
    if (! extend_child_environment(child_environment.data(),
                                   notification_environment, &extended,
                                   &environment_error)) {
      stop_notification_listener(service);
      return stop_service(service, 3);
    }
    child_environment = std::move(extended);
  }

  throttle_restart(service);

  if (WaitForSingleObject(service->stop_event, 0) == WAIT_OBJECT_0) {
    return kStartHandledByRecovery;
  }

  update_service_status(service, [](SERVICE_STATUS &status) {
    status.dwCurrentState = SERVICE_START_PENDING;
    status.dwControlsAccepted = SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_STOP;
  });

  if (! run_lifecycle_commands(
          service, service->config->lifecycle.exec_start_pre,
          L"ExecStartPre", child_environment.data(), service->stop_event,
          start_deadline, true)) {
    return stop_service(service, 3);
  }

  /* Did another thread receive a stop control? */
  if (restart_allowed(service)) {
    /* Set up I/O redirection. */
    if (get_output_handles(service, &si)) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GET_OUTPUT_HANDLES_FAILED, service->name, 0);
      FreeConsole();
      close_output_handles(&si);
      return stop_service(service, 4);
    }
    FreeConsole();

    if (! configure_service_job(service)) {
      close_output_handles(&si);
      return stop_service(service, 3);
    }
    if (! create_suspended_service_process(
            service, &si, &command_line, child_environment.data(), &pi)) {
      close_output_handles(&si);
      close_failed_child_launch(service, nullptr);
      return stop_service(service, 3);
    }
    if (! assign_service_process_to_job(service, pi.hThread)) {
      close_output_handles(&si);
      return stop_service(service, 3);
    }

    if (get_process_creation_time(service->process_handle, &service->creation_time)) ZeroMemory(&service->creation_time, sizeof(service->creation_time));

    close_output_handles(&si);

    if (! resume_service_process(service, pi.hThread)) {
      return stop_service(service, 3);
    }
  }

  return finish_service_start(service, child_environment.data(),
                              start_deadline);
}

/* Stop the service */
int stop_service(nssm_service_t *service, unsigned long exitcode) {
  set_restart_allowed(service, false);
  if (service->stop_event) SetEvent(service->stop_event);
  unregister_service_process_wait(service, true);
  critical_section_guard lifecycle_guard(&service->lifecycle_section);

  set_logger_rotation_state(&service->rotate_stdout_online,
                            NSSM_ROTATE_OFFLINE);
  set_logger_rotation_state(&service->rotate_stderr_online,
                            NSSM_ROTATE_OFFLINE);

  /* Signal we are stopping */
  update_service_status(service, [](SERVICE_STATUS &status) {
    status.dwCurrentState = SERVICE_STOP_PENDING;
    status.dwWaitHint = kServiceWaitHintMarginMs;
  });

  const lifecycle_config_t default_lifecycle;
  const lifecycle_config_t &lifecycle = service->config
                                              ? service->config->lifecycle
                                              : default_lifecycle;
  const unsigned long long stop_deadline =
      GetTickCount64() + lifecycle.timeout_stop_ms;
  std::vector<wchar_t> child_environment;
  std::wstring environment_error;
  const bool have_environment = build_child_environment(
      service->initial_env, service->env, service->env_extra,
      &child_environment, &environment_error);

  /* Nothing to do if service isn't running */
  if (service->pid) {
    if (have_environment) {
      run_lifecycle_commands(service, lifecycle.exec_stop,
                             L"ExecStop", child_environment.data(), nullptr,
                             stop_deadline, false);
    }
    const unsigned long remaining = remaining_until(stop_deadline);
    if (! lifecycle.exec_stop.empty() && remaining && service->process_handle &&
        WaitForSingleObject(service->process_handle, remaining) ==
            WAIT_OBJECT_0) {
      /* The requested stop command completed the shutdown. */
    }
    else {
    /* Shut down service */
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_TERMINATEPROCESS, service->name, service->exe, 0);
    kill_t k;
    service_kill_t(service, &k);
    k.exitcode = 0;
    const unsigned long grace = remaining_until(stop_deadline) / 3;
    k.kill_console_delay = grace;
    k.kill_window_delay = grace;
    k.kill_threads_delay = grace;
    kill_process(&k);
    }
  }
  else log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_PROCESS_ALREADY_STOPPED, service->name, service->exe, 0);

  end_service((void *) service, true);

  if (have_environment && ! service->stop_post_ran) {
    service->stop_post_ran = true;
    /*
      ExecStopPost runs after the main process has stopped, so it needs its
      own timeout budget.  Reusing stop_deadline meant a process which used
      the full shutdown grace period caused every post-stop command to be
      rejected as already timed out.
    */
    const unsigned long long stop_post_deadline =
        GetTickCount64() + lifecycle.timeout_stop_ms;
    run_lifecycle_commands(service, lifecycle.exec_stop_post,
                           L"ExecStopPost", child_environment.data(), nullptr,
                           stop_post_deadline, false);
  }

  /* Signal we stopped */
  update_service_status(service, [exitcode](SERVICE_STATUS &status) {
    status.dwCurrentState = SERVICE_STOPPED;
    if (exitcode) {
      status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
      status.dwServiceSpecificExitCode = exitcode;
    }
    else {
      status.dwWin32ExitCode = NO_ERROR;
      status.dwServiceSpecificExitCode = 0;
    }
  });

  if (exitcode && service->config) {
    unsigned long failure_error = ERROR_SUCCESS;
    if (! write_persistent_failure(service->name, exitcode, &failure_error)) {
      log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_PERSIST_FAILURE_FAILED,
                service->name, error_string(failure_error), 0);
    }
  }

  if (service->service_done_event) SetEvent(service->service_done_event);

  return exitcode;
}

/* Callback function triggered when the server exits */
void CALLBACK end_service(void *arg, unsigned char why) {
  nssm_service_t *service = (nssm_service_t *) arg;
  critical_section_guard lifecycle(&service->lifecycle_section);

  if (service->health_section_initialised) {
    EnterCriticalSection(&service->health_section);
    if (service->stopping) {
      LeaveCriticalSection(&service->health_section);
      return;
    }
    service->stopping = true;
    LeaveCriticalSection(&service->health_section);
  }
  else {
    if (service->stopping) return;
    service->stopping = true;
  }
  stop_resource_monitor(service);
  stop_health_monitor(service);
  stop_notification_listener(service);

  set_logger_rotation_state(&service->rotate_stdout_online,
                            NSSM_ROTATE_OFFLINE);
  set_logger_rotation_state(&service->rotate_stderr_online,
                            NSSM_ROTATE_OFFLINE);

  /* Use now as a dummy exit time. */
  GetSystemTimeAsFileTime(&service->exit_time);

  /* Check exit code */
  unsigned long exitcode = 0;
  wchar_t code[16];
  if (service->process_handle) {
    if (! GetExitCodeProcess(service->process_handle, &exitcode)) {
      const unsigned long error = GetLastError();
      exitcode = error ? error : ERROR_GEN_FAILURE;
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GETEXITCODEPROCESS_FAILED,
                service->name, error_string(exitcode), 0);
    }
    service->exitcode = exitcode;
    /* Check real exit time. */
    if (exitcode != STILL_ACTIVE) get_process_exit_time(service->process_handle, &service->exit_time);
    CloseHandle(service->process_handle);
  }

  service->process_handle = 0;

  /*
    Log that the service ended BEFORE logging about killing the process
    tree.  See below for the possible values of the why argument.
  */
  if (! why) {
    _snwprintf_s(code, _countof(code), _TRUNCATE, L"%lu", exitcode);
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_ENDED_SERVICE, service->exe, service->name, code, 0);
  }

  /* Clean up. */
  if (exitcode == STILL_ACTIVE) exitcode = 0;
  if (service->job_handle) {
    unsigned long job_error = ERROR_SUCCESS;
    if (! terminate_service_job(service->job_handle, exitcode, &job_error)) {
      log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_TERMINATE_JOB_FAILED,
                service->name, error_string(job_error), 0);
    }
    CloseHandle(service->job_handle);
    service->job_handle = 0;
  }
  else if (service->pid && service->kill_process_tree) {
    kill_t k;
    service_kill_t(service, &k);
    kill_process_tree(&k, service->pid);
  }
  service->pid = 0;
  publish_status_snapshot(service);

  /* Exit logging threads. */
  cleanup_loggers(service);
  unregister_service_process_wait(service, false);

  if (! why && service->config &&
      ! service->stop_post_ran &&
      ! service->config->lifecycle.exec_stop_post.empty()) {
    service->stop_post_ran = true;
    std::vector<wchar_t> environment;
    std::wstring error;
    if (build_child_environment(service->initial_env, service->env,
                                service->env_extra, &environment, &error)) {
      const unsigned long long deadline =
          GetTickCount64() + service->config->lifecycle.timeout_stop_ms;
      run_lifecycle_commands(service,
                             service->config->lifecycle.exec_stop_post,
                             L"ExecStopPost", environment.data(), nullptr,
                             deadline, false);
    }
  }

  /*
    The why argument is true if our wait timed out or false otherwise.
    Our wait is infinite so why will never be true when called by the system.
    If it is indeed true, assume we were called from stop_service() because
    this is a controlled shutdown, and don't take any restart action.
  */
  if (why) return;
  if (! restart_allowed(service)) return;

  unsigned long long now_ms = GetTickCount64();
  unsigned long long run_duration_ms =
      now_ms >= service->process_started_ms
          ? now_ms - service->process_started_ms
          : 0;
  LARGE_INTEGER counter = {};
  QueryPerformanceCounter(&counter);
  AcquireSRWLockExclusive(&service->status_snapshot_lock);
  recovery_decision_t decision = evaluate_recovery(
      service->config->recovery, service->recovery_state, exitcode, now_ms,
      run_duration_ms, static_cast<unsigned long>(counter.QuadPart));
  ReleaseSRWLockExclusive(&service->status_snapshot_lock);
  publish_status_snapshot(service);
  if (! decision.should_restart()) {
    const wchar_t *reason = decision.result == recovery_result_t::clean_exit
                              ? L"clean exit"
                          : decision.result == recovery_result_t::rate_limited
                              ? L"restart rate limit reached"
                              : L"recovery disabled";
    log_event(decision.result == recovery_result_t::clean_exit
                  ? EVENTLOG_INFORMATION_TYPE
                  : EVENTLOG_WARNING_TYPE,
              NSSM_EVENT_RECOVERY_STOPPED,
              service->name, reason, 0);
    if (decision.result != recovery_result_t::clean_exit) {
      unsigned long failure_error = ERROR_SUCCESS;
      if (! write_persistent_failure(service->name, exitcode,
                                     &failure_error)) {
        log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_PERSIST_FAILURE_FAILED,
                  service->name, error_string(failure_error), 0);
      }
    }
    stop_service(service, exitcode);
    return;
  }
  service->next_restart_delay_ms = decision.delay_ms;
  log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_RESTART,
            service->name, code, L"Restart",
            service->exe, 0);
  while (monitor_service(service)) {
    if (WaitForSingleObject(service->stop_event, 0) == WAIT_OBJECT_0) break;
    log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_RESTART_SERVICE_FAILED,
              service->exe, service->name, 0);
    if (WaitForSingleObject(service->stop_event, 30000) == WAIT_OBJECT_0) {
      break;
    }
  }
}

void throttle_restart(nssm_service_t *service) {
  unsigned long ms = service->next_restart_delay_ms;
  service->next_restart_delay_ms = 0;
  if (! ms) return;
  EnterCriticalSection(&service->throttle_section);
  ++service->throttle;
  update_service_status(service, [](SERVICE_STATUS &status) {
    status.dwCurrentState = SERVICE_PAUSED;
    status.dwControlsAccepted |= SERVICE_ACCEPT_PAUSE_CONTINUE;
  });
  SleepConditionVariableCS(&service->throttle_condition,
                           &service->throttle_section, ms);
  LeaveCriticalSection(&service->throttle_section);
}
