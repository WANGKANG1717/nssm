#include "nssm.h"
#include "account.h"
#include "child_process.h"
#include "config.h"
#include "console.h"
#include "env.h"
#include "event.h"
#include "job_object.h"
#include "health_monitor.h"
#include "messages.h"
#include "nssm_io.h"
#include "process.h"
#include "process_options.h"
#include "readiness.h"
#include "registry.h"
#include "service.h"
#include "status_snapshot.h"
#include "recovery.h"
#include "resource_monitor.h"

bool is_admin;
const TCHAR *startup_strings[] = { _T("SERVICE_AUTO_START"), _T("SERVICE_DELAYED_AUTO_START"), _T("SERVICE_DEMAND_START"), _T("SERVICE_DISABLED"), 0 };
const TCHAR *priority_strings[] = { _T("REALTIME_PRIORITY_CLASS"), _T("HIGH_PRIORITY_CLASS"), _T("ABOVE_NORMAL_PRIORITY_CLASS"), _T("NORMAL_PRIORITY_CLASS"), _T("BELOW_NORMAL_PRIORITY_CLASS"), _T("IDLE_PRIORITY_CLASS"), 0 };

static constexpr int kStartHandledByRecovery = -1;

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
                                wchar_t *environment) {
  if (service->config->readiness.enabled()) {
    readiness_result_t readiness = wait_for_readiness(
        service->config->readiness, service->process_handle, environment,
        service->dir, service->status_handle, &service->status);
    if (! readiness.ready()) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_READINESS_FAILED,
                service->name, readiness.detail.c_str(), 0);
      if (readiness.state != readiness_state_t::process_exited &&
          service->job_handle) {
        terminate_service_job(service->job_handle, ERROR_NOT_READY, nullptr);
        WaitForSingleObject(service->process_handle,
                            NSSM_SERVICE_STATUS_DEADLINE);
      }
      end_service(service, false);
      return kStartHandledByRecovery;
    }
    service->throttle = 0;
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_READINESS_READY,
              service->name, readiness.detail.c_str(), 0);
  }
  else if (await_single_handle(
               service->status_handle, &service->status,
               service->process_handle, service->name, _T("start_service"),
               NSSM_RESET_THROTTLE_RESTART) == 1) {
    service->throttle = 0;
  }

  if (! service->allow_restart) return 0;

  service->status.dwCurrentState = SERVICE_RUNNING;
  service->status.dwCheckPoint = 0;
  service->status.dwWaitHint = 0;
  service->status.dwControlsAccepted &= ~SERVICE_ACCEPT_PAUSE_CONTINUE;
  SetServiceStatus(service->status_handle, &service->status);
  publish_status_snapshot(service);
  return 0;
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

/* Set default values which aren't zero. */
void set_nssm_service_defaults(nssm_service_t *service) {
  if (! service) return;

  InitializeSRWLock(&service->status_snapshot_lock);
  service->type = SERVICE_WIN32_OWN_PROCESS;
  service->priority = NORMAL_PRIORITY_CLASS;
  service->stdin_sharing = NSSM_STDIN_SHARING;
  service->stdin_disposition = NSSM_STDIN_DISPOSITION;
  service->stdin_flags = NSSM_STDIN_FLAGS;
  service->stdout_sharing = NSSM_STDOUT_SHARING;
  service->stdout_disposition = NSSM_STDOUT_DISPOSITION;
  service->stdout_flags = NSSM_STDOUT_FLAGS;
  service->stderr_sharing = NSSM_STDERR_SHARING;
  service->stderr_disposition = NSSM_STDERR_DISPOSITION;
  service->stderr_flags = NSSM_STDERR_FLAGS;
  service->stop_method = ~0;
  service->kill_console_delay = NSSM_KILL_CONSOLE_GRACE_PERIOD;
  service->kill_window_delay = NSSM_KILL_WINDOW_GRACE_PERIOD;
  service->kill_threads_delay = NSSM_KILL_THREADS_GRACE_PERIOD;
  service->kill_process_tree = 1;
}

/* Allocate and zero memory for a service. */
nssm_service_t *alloc_nssm_service() {
  nssm_service_t *service = (nssm_service_t *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(nssm_service_t));
  if (! service) log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("service"), _T("alloc_nssm_service()"), 0);
  return service;
}

/* Free memory for a service. */
void cleanup_nssm_service(nssm_service_t *service) {
  if (! service) return;
  stop_resource_monitor(service);
  stop_health_monitor(service);
  close_status_snapshot(service);
  if (service->username) HeapFree(GetProcessHeap(), 0, service->username);
  if (service->password) {
    SecureZeroMemory(service->password, service->passwordlen * sizeof(TCHAR));
    HeapFree(GetProcessHeap(), 0, service->password);
  }
  if (service->dependencies) HeapFree(GetProcessHeap(), 0, service->dependencies);
  if (service->env) HeapFree(GetProcessHeap(), 0, service->env);
  if (service->env_extra) HeapFree(GetProcessHeap(), 0, service->env_extra);
  if (service->handle) CloseServiceHandle(service->handle);
  if (service->process_handle) CloseHandle(service->process_handle);
  if (service->job_handle) CloseHandle(service->job_handle);
  if (service->wait_handle) UnregisterWait(service->wait_handle);
  if (service->throttle_section_initialised) DeleteCriticalSection(&service->throttle_section);
  if (service->health_section_initialised) DeleteCriticalSection(&service->health_section);
  if (service->resource_section_initialised) DeleteCriticalSection(&service->resource_section);
  if (service->initial_env) HeapFree(GetProcessHeap(), 0, service->initial_env);
  delete service->config;
  delete service->recovery_state;
  HeapFree(GetProcessHeap(), 0, service);
}

/* About to install the service */
/* Service initialisation */
void WINAPI service_main(unsigned long argc, TCHAR **argv) {
  nssm_service_t *service = alloc_nssm_service();
  if (! service) return;
  set_nssm_service_defaults(service);

  static volatile bool await_debugger = (argc > 1 && str_equiv(argv[1], _T("debug")));
  while (await_debugger) Sleep(1000);

  if (_sntprintf_s(service->name, _countof(service->name), _TRUNCATE, _T("%s"), argv[0]) < 0) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("service->name"), _T("service_main()"), 0);
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
  service->status.dwWaitHint = NSSM_WAITHINT_MARGIN;

  /* Signal we AREN'T running the server */
  service->process_handle = 0;
  service->pid = 0;

  /* Register control handler */
  service->status_handle = RegisterServiceCtrlHandlerEx(NSSM, service_control_handler, (void *) service);
  if (! service->status_handle) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_REGISTERSERVICECTRLHANDER_FAILED, error_string(GetLastError()), 0);
    return;
  }

  log_service_control(service->name, 0, true);

  service->status.dwCurrentState = SERVICE_START_PENDING;
  service->status.dwWaitHint = NSSM_RESET_THROTTLE_RESTART + NSSM_WAITHINT_MARGIN;
  SetServiceStatus(service->status_handle, &service->status);

  if (is_admin) {
    SC_HANDLE services = open_service_manager(SC_MANAGER_CONNECT);
    if (services) {
      service->handle = open_service(services, service->name, SERVICE_CHANGE_CONFIG, 0, 0);
      set_service_recovery(service);

      /* Remember our display name. */
      unsigned long displayname_len = _countof(service->displayname);
      GetServiceDisplayName(services, service->name, service->displayname, &displayname_len);

      CloseServiceHandle(services);
    }
  }

  /* Used for signalling a resume if the service pauses when throttled. */
  InitializeCriticalSection(&service->throttle_section);
  InitializeConditionVariable(&service->throttle_condition);
  service->throttle_section_initialised = true;

  /* Serialise health monitor startup and shutdown with process exit. */
  InitializeCriticalSection(&service->health_section);
  service->health_section_initialised = true;
  InitializeCriticalSection(&service->resource_section);
  service->resource_section_initialised = true;

  /* Remember our initial environment. */
  service->initial_env = copy_environment();

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
    return;
  }
  if (! create_status_snapshot(service)) {
    log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_STATUS_SNAPSHOT_FAILED,
              service->name, error_string(GetLastError()), 0);
  }

  /* Remember our creation time. */
  if (get_process_creation_time(GetCurrentProcess(), &service->nssm_creation_time)) ZeroMemory(&service->nssm_creation_time, sizeof(service->nssm_creation_time));

  service->allow_restart = true;
  if (! CreateThread(NULL, 0, launch_service, (void *) service, 0, NULL)) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED, error_string(GetLastError()), 0);
    stop_service(service, 0);
  }
}

/* Make sure service recovery actions are taken where necessary */
void set_service_recovery(nssm_service_t *service) {
  SERVICE_FAILURE_ACTIONS_FLAG flag;
  ZeroMemory(&flag, sizeof(flag));
  flag.fFailureActionsOnNonCrashFailures = true;

  if (! ChangeServiceConfig2(service->handle, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag)) {
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
  if (ret) {
    TCHAR code[16];
    _sntprintf_s(code, _countof(code), _TRUNCATE, _T("%d"), ret);
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_START_SERVICE_FAILED,
              service->exe, service->name, code, 0);
    return ret;
  }
  log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_STARTED_SERVICE, service->exe, service->flags, service->name, service->dir, 0);

  /* Monitor service */
  if (! RegisterWaitForSingleObject(&service->wait_handle, service->process_handle, end_service, (void *) service, INFINITE, WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION)) {
    log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_REGISTERWAITFORSINGLEOBJECT_FAILED, service->name, service->exe, error_string(GetLastError()), 0);
  }
  else {
    if (! start_health_monitor(service)) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_HEALTH_MONITOR_START_FAILED,
                service->name, error_string(GetLastError()), 0);
      if (service->job_handle) {
        terminate_service_job(service->job_handle, ERROR_NOT_READY, nullptr);
      }
    }
    else if (! start_resource_monitor(service)) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_RESOURCE_MONITOR_START_FAILED,
                service->name, error_string(GetLastError()), 0);
      if (service->job_handle) {
        terminate_service_job(service->job_handle, ERROR_NOT_READY, nullptr);
      }
    }
  }

  return 0;
}

TCHAR *service_control_text(unsigned long control) {
  switch (control) {
    /* HACK: there is no SERVICE_CONTROL_START constant */
    case NSSM_SERVICE_CONTROL_START: return _T("START");
    case SERVICE_CONTROL_STOP: return _T("STOP");
    case SERVICE_CONTROL_SHUTDOWN: return _T("SHUTDOWN");
    case SERVICE_CONTROL_PAUSE: return _T("PAUSE");
    case SERVICE_CONTROL_CONTINUE: return _T("CONTINUE");
    case SERVICE_CONTROL_INTERROGATE: return _T("INTERROGATE");
    case NSSM_SERVICE_CONTROL_ROTATE: return _T("ROTATE");
    case SERVICE_CONTROL_POWEREVENT: return _T("POWEREVENT");
    default: return 0;
  }
}

TCHAR *service_status_text(unsigned long status) {
  switch (status) {
    case SERVICE_STOPPED: return _T("SERVICE_STOPPED");
    case SERVICE_START_PENDING: return _T("SERVICE_START_PENDING");
    case SERVICE_STOP_PENDING: return _T("SERVICE_STOP_PENDING");
    case SERVICE_RUNNING: return _T("SERVICE_RUNNING");
    case SERVICE_CONTINUE_PENDING: return _T("SERVICE_CONTINUE_PENDING");
    case SERVICE_PAUSE_PENDING: return _T("SERVICE_PAUSE_PENDING");
    case SERVICE_PAUSED: return _T("SERVICE_PAUSED");
    default: return 0;
  }
}

void log_service_control(TCHAR *service_name, unsigned long control, bool handled) {
  TCHAR *text = service_control_text(control);
  unsigned long event;

  if (! text) {
    /* "0x" + 8 x hex + NULL */
    text = (TCHAR *) HeapAlloc(GetProcessHeap(), 0, 11 * sizeof(TCHAR));
    if (! text) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("control code"), _T("log_service_control()"), 0);
      return;
    }
    if (_sntprintf_s(text, 11, _TRUNCATE, _T("0x%08x"), control) < 0) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("control code"), _T("log_service_control()"), 0);
      HeapFree(GetProcessHeap(), 0, text);
      return;
    }

    event = NSSM_EVENT_SERVICE_CONTROL_UNKNOWN;
  }
  else if (handled) event = NSSM_EVENT_SERVICE_CONTROL_HANDLED;
  else event = NSSM_EVENT_SERVICE_CONTROL_NOT_HANDLED;

  log_event(EVENTLOG_INFORMATION_TYPE, event, service_name, text, 0);

  if (event == NSSM_EVENT_SERVICE_CONTROL_UNKNOWN) {
    HeapFree(GetProcessHeap(), 0, text);
  }
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
      service->last_control = control;
      log_service_control(service->name, control, true);

      /* Immediately block further controls. */
      service->allow_restart = false;
      WakeConditionVariable(&service->throttle_condition);
      service->status.dwCurrentState = SERVICE_STOP_PENDING;
      service->status.dwControlsAccepted = 0;
      SetServiceStatus(service->status_handle, &service->status);

      /*
        We MUST acknowledge the stop request promptly but we're committed to
        waiting for the application to exit.  Spawn a new thread to wait
        while we acknowledge the request.
      */
      if (! CreateThread(NULL, 0, shutdown_service, context, 0, NULL)) {
        log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED, error_string(GetLastError()), 0);

        /*
          We couldn't create a thread to tidy up so we'll have to force the tidyup
          to complete in time in this thread.
        */
        service->kill_console_delay = NSSM_KILL_CONSOLE_GRACE_PERIOD;
        service->kill_window_delay = NSSM_KILL_WINDOW_GRACE_PERIOD;
        service->kill_threads_delay = NSSM_KILL_THREADS_GRACE_PERIOD;

        stop_service(service, 0);
      }
      return NO_ERROR;

    case SERVICE_CONTROL_CONTINUE:
      service->last_control = control;
      log_service_control(service->name, control, true);
      service->throttle = 0;
      WakeConditionVariable(&service->throttle_condition);
      /* We can't continue if the application is running! */
      if (! service->process_handle) service->status.dwCurrentState = SERVICE_CONTINUE_PENDING;
      service->status.dwWaitHint = throttle_milliseconds(service->throttle) + NSSM_WAITHINT_MARGIN;
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_RESET_THROTTLE, service->name, 0);
      SetServiceStatus(service->status_handle, &service->status);
      return NO_ERROR;

    case SERVICE_CONTROL_PAUSE:
      /*
        We don't accept pause messages but it isn't possible to register
        only for continue messages so we have to handle this case.
      */
      log_service_control(service->name, control, false);
      return ERROR_CALL_NOT_IMPLEMENTED;

    case NSSM_SERVICE_CONTROL_ROTATE:
      service->last_control = control;
      log_service_control(service->name, control, true);
      if (service->rotate_stdout_online == NSSM_ROTATE_ONLINE) service->rotate_stdout_online = NSSM_ROTATE_ONLINE_ASAP;
      if (service->rotate_stderr_online == NSSM_ROTATE_ONLINE) service->rotate_stderr_online = NSSM_ROTATE_ONLINE_ASAP;
      return NO_ERROR;
  }

  /* Unknown control */
  log_service_control(service->name, control, false);
  return ERROR_CALL_NOT_IMPLEMENTED;
}

/* Start the service */
int start_service(nssm_service_t *service) {
  if (service->health_section_initialised) {
    EnterCriticalSection(&service->health_section);
    service->stopping = false;
    LeaveCriticalSection(&service->health_section);
  }
  else service->stopping = false;

  if (service->process_handle) return 0;
  /* Allocate a STARTUPINFO structure for a new process */
  STARTUPINFO si;
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

  throttle_restart(service);

  service->status.dwCurrentState = SERVICE_START_PENDING;
  service->status.dwControlsAccepted = SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_STOP;
  SetServiceStatus(service->status_handle, &service->status);

  /* Did another thread receive a stop control? */
  if (service->allow_restart) {
    /* Set up I/O redirection. */
    if (get_output_handles(service, &si)) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GET_OUTPUT_HANDLES_FAILED, service->name, 0);
      FreeConsole();
      close_output_handles(&si);
      return stop_service(service, 4);
    }
    FreeConsole();

    bool inherit_handles = false;
    if (si.dwFlags & STARTF_USESTDHANDLES) inherit_handles = true;
    unsigned long flags = service->priority & priority_mask();
    flags |= CREATE_SUSPENDED;
    if (! service->no_console) flags |= CREATE_NEW_CONSOLE;
    flags |= CREATE_UNICODE_ENVIRONMENT;

    unsigned long job_error = ERROR_SUCCESS;
    service->job_handle = create_service_job(&job_error);
    if (! service->job_handle) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATE_JOB_FAILED, service->name,
                error_string(job_error), 0);
      close_output_handles(&si);
      return stop_service(service, 3);
    }
    if (service->config->resources.enabled() &&
        ! configure_service_job_limits(
            service->job_handle,
            service->config->resources.maximum_processes,
            service->config->resources.memory_limit_bytes, &job_error)) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CONFIGURE_JOB_FAILED,
                service->name, error_string(job_error), 0);
      CloseHandle(service->job_handle);
      service->job_handle = 0;
      close_output_handles(&si);
      return stop_service(service, 3);
    }

    if (! CreateProcess(service->exe, command_line.data(), 0, 0, inherit_handles,
                        flags, child_environment.data(), service->dir, &si, &pi)) {
      unsigned long exitcode = 3;
      unsigned long error = GetLastError();
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEPROCESS_FAILED, service->name, service->exe, error_string(error), 0);
      close_output_handles(&si);
      CloseHandle(service->job_handle);
      service->job_handle = 0;
      return stop_service(service, exitcode);
    }
    service->process_handle = pi.hProcess;
    service->pid = pi.dwProcessId;
    service->process_started_ms = GetTickCount64();
    publish_status_snapshot(service);

    if (! assign_process_to_service_job(service->job_handle,
                                        service->process_handle, &job_error)) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_ASSIGN_JOB_FAILED, service->name,
                error_string(job_error), 0);
      TerminateProcess(service->process_handle, job_error);
      WaitForSingleObject(service->process_handle, NSSM_SERVICE_STATUS_DEADLINE);
      if (pi.hThread) CloseHandle(pi.hThread);
      CloseHandle(service->process_handle);
      service->process_handle = 0;
      service->pid = 0;
      CloseHandle(service->job_handle);
      service->job_handle = 0;
      close_output_handles(&si);
      return stop_service(service, 3);
    }

    if (get_process_creation_time(service->process_handle, &service->creation_time)) ZeroMemory(&service->creation_time, sizeof(service->creation_time));

    close_output_handles(&si);

    if (service->affinity) {
      /*
        We are explicitly storing service->affinity as a 64-bit unsigned integer
        so that we can parse it regardless of whether we're running in 32-bit
        or 64-bit mode.  The arguments to SetProcessAffinityMask(), however, are
        defined as type DWORD_PTR and hence limited to 32 bits on a 32-bit system
        (or when running the 32-bit NSSM).

        The result is a lot of seemingly-unnecessary casting throughout the code
        and potentially confusion when we actually try to start the service.
        Having said that, however, it's unlikely that we're actually going to
        run in 32-bit mode on a system which has more than 32 CPUs so the
        likelihood of seeing a confusing situation is somewhat diminished.
      */
      DWORD_PTR affinity, system_affinity;

      if (GetProcessAffinityMask(service->process_handle, &affinity, &system_affinity)) affinity = service->affinity & system_affinity;
      else {
        affinity = (DWORD_PTR) service->affinity;
        log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GETPROCESSAFFINITYMASK_FAILED, service->name, error_string(GetLastError()), 0);
      }

      if (! SetProcessAffinityMask(service->process_handle, affinity)) {
        log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_SETPROCESSAFFINITYMASK_FAILED, service->name, error_string(GetLastError()), 0);
      }

    }
    if (ResumeThread(pi.hThread) == static_cast<unsigned long>(-1)) {
      unsigned long resume_error = GetLastError();
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_RESUME_PROCESS_FAILED,
                service->name, error_string(resume_error), 0);
      terminate_service_job(service->job_handle, resume_error, nullptr);
      WaitForSingleObject(service->process_handle, NSSM_SERVICE_STATUS_DEADLINE);
      CloseHandle(pi.hThread);
      CloseHandle(service->process_handle);
      service->process_handle = 0;
      service->pid = 0;
      CloseHandle(service->job_handle);
      service->job_handle = 0;
      return stop_service(service, 3);
    }
    /* We never use the primary thread handle beyond ResumeThread(). */
    if (pi.hThread) CloseHandle(pi.hThread);
  }

  return finish_service_start(service, child_environment.data());
}

/* Stop the service */
int stop_service(nssm_service_t *service, unsigned long exitcode) {
  service->allow_restart = false;
  if (service->wait_handle) {
    UnregisterWait(service->wait_handle);
    service->wait_handle = 0;
  }

  service->rotate_stdout_online = service->rotate_stderr_online = NSSM_ROTATE_OFFLINE;

  /* Signal we are stopping */
  service->status.dwCurrentState = SERVICE_STOP_PENDING;
  service->status.dwWaitHint = NSSM_WAITHINT_MARGIN;
  SetServiceStatus(service->status_handle, &service->status);

  /* Nothing to do if service isn't running */
  if (service->pid) {
    /* Shut down service */
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_TERMINATEPROCESS, service->name, service->exe, 0);
    kill_t k;
    service_kill_t(service, &k);
    k.exitcode = 0;
    kill_process(&k);
  }
  else log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_PROCESS_ALREADY_STOPPED, service->name, service->exe, 0);

  end_service((void *) service, true);

  /* Signal we stopped */
  service->status.dwCurrentState = SERVICE_STOPPED;
  if (exitcode) {
    service->status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    service->status.dwServiceSpecificExitCode = exitcode;
  }
  else {
    service->status.dwWin32ExitCode = NO_ERROR;
    service->status.dwServiceSpecificExitCode = 0;
  }
  SetServiceStatus(service->status_handle, &service->status);

  return exitcode;
}

/* Callback function triggered when the server exits */
void CALLBACK end_service(void *arg, unsigned char why) {
  nssm_service_t *service = (nssm_service_t *) arg;

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

  service->rotate_stdout_online = service->rotate_stderr_online = NSSM_ROTATE_OFFLINE;

  /* Use now as a dummy exit time. */
  GetSystemTimeAsFileTime(&service->exit_time);

  /* Check exit code */
  unsigned long exitcode = 0;
  TCHAR code[16];
  if (service->process_handle) {
    GetExitCodeProcess(service->process_handle, &exitcode);
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
    _sntprintf_s(code, _countof(code), _TRUNCATE, _T("%lu"), exitcode);
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

  /*
    The why argument is true if our wait timed out or false otherwise.
    Our wait is infinite so why will never be true when called by the system.
    If it is indeed true, assume we were called from stop_service() because
    this is a controlled shutdown, and don't take any restart action.
  */
  if (why) return;
  if (! service->allow_restart) return;

  unsigned long long now_ms = GetTickCount64();
  unsigned long long run_duration_ms =
      now_ms >= service->process_started_ms
          ? now_ms - service->process_started_ms
          : 0;
  LARGE_INTEGER counter = {};
  QueryPerformanceCounter(&counter);
  recovery_decision_t decision = evaluate_recovery(
      service->config->recovery, service->recovery_state, exitcode, now_ms,
      run_duration_ms, static_cast<unsigned long>(counter.QuadPart));
  publish_status_snapshot(service);
  if (! decision.should_restart()) {
    const TCHAR *reason = decision.result == recovery_result_t::clean_exit
                              ? _T("clean exit")
                          : decision.result == recovery_result_t::rate_limited
                              ? _T("restart rate limit reached")
                              : _T("recovery disabled");
    log_event(decision.result == recovery_result_t::clean_exit
                  ? EVENTLOG_INFORMATION_TYPE
                  : EVENTLOG_WARNING_TYPE,
              NSSM_EVENT_RECOVERY_STOPPED,
              service->name, reason, 0);
    stop_service(service, exitcode);
    return;
  }
  service->next_restart_delay_ms = decision.delay_ms;
  log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_EXIT_RESTART,
            service->name, code, _T("Restart"),
            service->exe, 0);
  while (monitor_service(service)) {
    log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_RESTART_SERVICE_FAILED,
              service->exe, service->name, 0);
    Sleep(30000);
  }
}

void throttle_restart(nssm_service_t *service) {
  unsigned long ms = service->next_restart_delay_ms;
  service->next_restart_delay_ms = 0;
  if (! ms) return;
  ++service->throttle;
  EnterCriticalSection(&service->throttle_section);
  service->status.dwCurrentState = SERVICE_PAUSED;
  service->status.dwControlsAccepted |= SERVICE_ACCEPT_PAUSE_CONTINUE;
  SetServiceStatus(service->status_handle, &service->status);
  SleepConditionVariableCS(&service->throttle_condition,
                           &service->throttle_section, ms);
  LeaveCriticalSection(&service->throttle_section);
}

/*
  When responding to a stop (or any other) request we need to set dwWaitHint to
  the number of milliseconds we expect the operation to take, and optionally
  increase dwCheckPoint.  If dwWaitHint milliseconds elapses without the
  operation completing or dwCheckPoint increasing, the system will consider the
  service to be hung.

  However the system will consider the service to be hung after 30000
  milliseconds regardless of the value of dwWaitHint if dwCheckPoint has not
  changed.  Therefore if we want to wait longer than that we must periodically
  increase dwCheckPoint.

  Furthermore, it will consider the service to be hung after 60000 milliseconds
  regardless of the value of dwCheckPoint unless dwWaitHint is increased every
  time dwCheckPoint is also increased.

  Our strategy then is to retrieve the initial dwWaitHint and wait for
  NSSM_SERVICE_STATUS_DEADLINE milliseconds.  If the process is still running
  and we haven't finished waiting we increment dwCheckPoint and add whichever is
  smaller of NSSM_SERVICE_STATUS_DEADLINE or the remaining timeout to
  dwWaitHint.

  Only doing both these things will prevent the system from killing the service.

  If the status_handle and service_status arguments are omitted, this function
  will not try to update the service manager but it will still log to the
  event log that it is waiting for a handle.

  Returns: 1 if the wait timed out.
           0 if the wait completed.
          -1 on error.
*/
int await_single_handle(SERVICE_STATUS_HANDLE status_handle, SERVICE_STATUS *status, HANDLE handle, TCHAR *name, TCHAR *function_name, unsigned long timeout) {
  unsigned long interval;
  unsigned long ret;
  unsigned long waited;
  TCHAR interval_milliseconds[16];
  TCHAR timeout_milliseconds[16];
  TCHAR waited_milliseconds[16];
  TCHAR *function = function_name;

  /* Add brackets to function name. */
  size_t funclen = _tcslen(function_name) + 3;
  TCHAR *func = (TCHAR *) HeapAlloc(GetProcessHeap(), 0, funclen * sizeof(TCHAR));
  if (func) {
    if (_sntprintf_s(func, funclen, _TRUNCATE, _T("%s()"), function_name) > -1) function = func;
  }

  _sntprintf_s(timeout_milliseconds, _countof(timeout_milliseconds), _TRUNCATE, _T("%lu"), timeout);

  waited = 0;
  while (waited < timeout) {
    interval = timeout - waited;
    if (interval > NSSM_SERVICE_STATUS_DEADLINE) interval = NSSM_SERVICE_STATUS_DEADLINE;

    if (status) {
      status->dwWaitHint += interval;
      status->dwCheckPoint++;
      SetServiceStatus(status_handle, status);
    }

    if (waited) {
      _sntprintf_s(waited_milliseconds, _countof(waited_milliseconds), _TRUNCATE, _T("%lu"), waited);
      _sntprintf_s(interval_milliseconds, _countof(interval_milliseconds), _TRUNCATE, _T("%lu"), interval);
      log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_AWAITING_SINGLE_HANDLE, function, name, waited_milliseconds, interval_milliseconds, timeout_milliseconds, 0);
    }

    switch (WaitForSingleObject(handle, interval)) {
      case WAIT_OBJECT_0:
        ret = 0;
        goto awaited;

      case WAIT_TIMEOUT:
        ret = 1;
        break;

      default:
        ret = -1;
        goto awaited;
    }

    waited += interval;
  }

awaited:
  if (func) HeapFree(GetProcessHeap(), 0, func);

  return ret;
}
