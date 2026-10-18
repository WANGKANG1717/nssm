#include "event.h"
#include "messages.h"
#include "process.h"
#include "service.h"
#include "service_wait.h"
#include "win32_handle.h"

void service_kill_t(nssm_service_t *service, kill_t *k) {
  if (! service) return;
  if (! k) return;

  ZeroMemory(k, sizeof(*k));
  k->name = service->name;
  k->process_handle = service->process_handle;
  k->pid = service->pid;
  k->exitcode = service->exitcode;
  k->kill_console_delay = service->kill_console_delay;
  k->kill_window_delay = service->kill_window_delay;
  k->kill_threads_delay = service->kill_threads_delay;
  k->status_handle = service->status_handle;
  k->status = &service->status;
  k->status_lock = &service->status_lock;
  k->creation_time = service->creation_time;
  k->exit_time = service->exit_time;
}

int get_process_creation_time(HANDLE process_handle, FILETIME *ft) {
  FILETIME creation_time, exit_time, kernel_time, user_time;

  if (! GetProcessTimes(process_handle, &creation_time, &exit_time, &kernel_time, &user_time)) {
    const unsigned long error = GetLastError();
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GETPROCESSTIMES_FAILED,
              error_string(error), 0);
    SetLastError(error);
    return 1;
  }

  memmove(ft, &creation_time, sizeof(creation_time));

  return 0;
}

int get_process_exit_time(HANDLE process_handle, FILETIME *ft) {
  FILETIME creation_time, exit_time, kernel_time, user_time;

  if (! GetProcessTimes(process_handle, &creation_time, &exit_time, &kernel_time, &user_time)) {
    const unsigned long error = GetLastError();
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GETPROCESSTIMES_FAILED,
              error_string(error), 0);
    SetLastError(error);
    return 1;
  }

  if (! (exit_time.dwLowDateTime || exit_time.dwHighDateTime)) return 2;
  memmove(ft, &exit_time, sizeof(exit_time));

  return 0;
}

int check_parent(kill_t *k, PROCESSENTRY32W *pe, unsigned long ppid) {
  /* Check parent process ID matches. */
  if (pe->th32ParentProcessID != ppid) return 1;

  /*
    Process IDs can be reused so do a sanity check by making sure the child
    has been running for less time than the parent.
    Though unlikely, it's possible that the parent exited and its process ID
    was already reused, so we'll also compare against its exit time.
  */
  unique_handle process_handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                            false, pe->th32ProcessID));
  if (! process_handle) {
    const unsigned long error = GetLastError();
    if (! k->traversal_error) k->traversal_error = error;
    wchar_t pid_string[16];
    _snwprintf_s(pid_string, _countof(pid_string), _TRUNCATE, L"%lu", pe->th32ProcessID);
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENPROCESS_FAILED, pid_string,
              k->name, error_string(error), 0);
    return 2;
  }

  FILETIME ft;
  if (get_process_creation_time(process_handle.get(), &ft)) {
    if (! k->traversal_error) k->traversal_error = GetLastError();
    return 3;
  }

  /* Verify that the parent's creation time is not later. */
  if (CompareFileTime(&k->creation_time, &ft) > 0) return 4;

  /* Verify that the parent's exit time is not earlier. */
  if (CompareFileTime(&k->exit_time, &ft) < 0) return 5;

  return 0;
}

/* Send some window messages and hope the window respects one or more. */
int CALLBACK kill_window(HWND window, LPARAM arg) {
  kill_t *k = (kill_t *) arg;

  unsigned long pid;
  if (! GetWindowThreadProcessId(window, &pid)) return 1;
  if (pid != k->pid) return 1;

  /* First try sending WM_CLOSE to request that the window close. */
  k->signalled |= PostMessage(window, WM_CLOSE, k->exitcode, 0);

  /*
    Then tell the window that the user is logging off and it should exit
    without worrying about saving any data.
  */
  k->signalled |= PostMessage(window, WM_ENDSESSION, 1, ENDSESSION_CLOSEAPP | ENDSESSION_CRITICAL | ENDSESSION_LOGOFF);

  return 1;
}

/*
  Try to post a message to the message queues of threads associated with the
  given process ID.  Not all threads have message queues so there's no
  guarantee of success, and we don't want to be left waiting for unsignalled
  processes so this function returns only true if at least one thread was
  successfully prodded.
*/
int kill_threads(nssm_service_t *service, kill_t *k) {
  (void) service;
  int ret = 0;

  /* Get a snapshot of all threads in the system. */
  unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
  if (! snapshot) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETOOLHELP32SNAPSHOT_THREAD_FAILED, k->name, error_string(GetLastError()), 0);
    return 0;
  }

  THREADENTRY32 te;
  ZeroMemory(&te, sizeof(te));
  te.dwSize = sizeof(te);

  if (! Thread32First(snapshot.get(), &te)) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_THREAD_ENUMERATE_FAILED, k->name, error_string(GetLastError()), 0);
    return 0;
  }

  /* This thread belongs to the doomed process so signal it. */
  if (te.th32OwnerProcessID == k->pid) {
    ret |= PostThreadMessage(te.th32ThreadID, WM_QUIT, k->exitcode, 0);
  }

  while (true) {
    /* Try to get the next thread. */
    if (! Thread32Next(snapshot.get(), &te)) {
      unsigned long error = GetLastError();
      if (error == ERROR_NO_MORE_FILES) break;
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_THREAD_ENUMERATE_FAILED,
                k->name, error_string(error), 0);
      return ret;
    }

    if (te.th32OwnerProcessID == k->pid) {
      ret |= PostThreadMessage(te.th32ThreadID, WM_QUIT, k->exitcode, 0);
    }
  }

  return ret;
}

int kill_threads(kill_t *k) {
  return kill_threads(NULL, k);
}

/* Give the process a chance to die gracefully. */
int kill_process(nssm_service_t *service, kill_t *k) {
  (void) service;
  if (! k) return 1;

  unsigned long ret;
  if (GetExitCodeProcess(k->process_handle, &ret)) {
    if (ret != STILL_ACTIVE) return 1;
  }

  /* Try to send a Control-C event to the console. */
  if (! kill_console(k)) return 1;

  /*
    Try to post messages to the windows belonging to the given process ID.
    If the process is a console application it won't have any windows so there's
    no guarantee of success.
  */
  EnumWindows((WNDENUMPROC) kill_window, (LPARAM) k);
  if (k->signalled) {
    if (! await_single_handle(k->status_handle, k->status, k->process_handle,
                              k->name, L"kill_process",
                              k->kill_window_delay, k->status_lock)) return 1;
    k->signalled = 0;
  }

  /*
    Try to post messages to any thread message queues associated with the
    process.  Console applications might have them (but probably won't) so
    there's still no guarantee of success.
  */
  if (kill_threads(k)) {
    if (! await_single_handle(k->status_handle, k->status, k->process_handle,
                              k->name, L"kill_process",
                              k->kill_threads_delay, k->status_lock)) return 1;
  }

  /* We tried being nice.  Time for extreme prejudice. */
  return TerminateProcess(k->process_handle, k->exitcode);
}

int kill_process(kill_t *k) {
  return kill_process(NULL, k);
}

/* Simulate a Control-C event to our console (shared with the app). */
int kill_console(nssm_service_t *service, kill_t *k) {
  (void) service;
  unsigned long ret;

  if (! k) return 1;

  /* Try to attach to the process's console. */
  if (! AttachConsole(k->pid)) {
    ret = GetLastError();

    switch (ret) {
      case ERROR_INVALID_HANDLE:
        /* The app doesn't have a console. */
        return 1;

      case ERROR_GEN_FAILURE:
        /* The app already exited. */
        return 2;

      case ERROR_ACCESS_DENIED:
      default:
        /* We already have a console. */
        log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_ATTACHCONSOLE_FAILED, k->name, error_string(ret), 0);
        return 3;
    }
  }

  /* Ignore the event ourselves. */
  ret = 0;
  BOOL ignored = SetConsoleCtrlHandler(0, TRUE);
  if (! ignored) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETCONSOLECTRLHANDLER_FAILED, k->name, error_string(GetLastError()), 0);
    ret = 4;
  }

  /* Send the event. */
  if (! ret) {
    if (! GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_GENERATECONSOLECTRLEVENT_FAILED, k->name, error_string(GetLastError()), 0);
      ret = 5;
    }
  }

  /* Detach from the console. */
  if (! FreeConsole()) {
    log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_FREECONSOLE_FAILED, k->name, error_string(GetLastError()), 0);
  }

  /* Wait for process to exit. */
  if (await_single_handle(k->status_handle, k->status, k->process_handle,
                          k->name, L"kill_console",
                          k->kill_console_delay, k->status_lock)) ret = 6;

  /* Remove our handler. */
  if (ignored && ! SetConsoleCtrlHandler(0, FALSE)) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETCONSOLECTRLHANDLER_FAILED, k->name, error_string(GetLastError()), 0);
  }

  return ret;
}

int kill_console(kill_t *k) {
  return kill_console(NULL, k);
}

void walk_process_tree(nssm_service_t *service, walk_function_t fn, kill_t *k, unsigned long ppid) {
  if (! k) return;
  /* Shouldn't happen unless the service failed to start. */
  /* PID 0 is the System Idle Process and must never enter the stop pipeline. */
  if (! k->pid) return;
  unsigned long pid = k->pid;
  unsigned long depth = k->depth;

  wchar_t pid_string[16], code[16];
  _snwprintf_s(pid_string, _countof(pid_string), _TRUNCATE, L"%lu", pid);
  _snwprintf_s(code, _countof(code), _TRUNCATE, L"%lu", k->exitcode);
  if (fn == static_cast<walk_function_t>(kill_process)) log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_KILLING, k->name, pid_string, code, 0);

  /* We will need a process handle in order to call TerminateProcess() later. */
  unique_handle process_handle(OpenProcess(
      SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
      false, pid));
  if (process_handle) {
    /* Kill this process first, then its descendents. */
    wchar_t ppid_string[16];
    _snwprintf_s(ppid_string, _countof(ppid_string), _TRUNCATE, L"%lu", ppid);
    if (fn == static_cast<walk_function_t>(kill_process)) log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_KILL_PROCESS_TREE, pid_string, ppid_string, k->name, 0);
    HANDLE previous_process_handle = k->process_handle;
    k->process_handle = process_handle.get();
    if (! fn(service, k)) {
      const unsigned long action_error = GetLastError();
      /* Maybe it already died. */
      unsigned long ret;
      if (! GetExitCodeProcess(process_handle.get(), &ret) ||
          ret == STILL_ACTIVE) {
        log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_TERMINATEPROCESS_FAILED,
                  pid_string, k->name, error_string(action_error), 0);
      }
    }

    k->process_handle = previous_process_handle;
  }
  else {
    const unsigned long error = GetLastError();
    if (! k->traversal_error) k->traversal_error = error;
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENPROCESS_FAILED, pid_string,
              k->name, error_string(error), 0);
  }

  /* Get a snapshot of all processes in the system. */
  unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (! snapshot) {
    const unsigned long error = GetLastError();
    if (! k->traversal_error) k->traversal_error = error;
    log_event(EVENTLOG_ERROR_TYPE,
              NSSM_EVENT_CREATETOOLHELP32SNAPSHOT_PROCESS_FAILED, k->name,
              error_string(error), 0);
    return;
  }

  PROCESSENTRY32W pe;
  ZeroMemory(&pe, sizeof(pe));
  pe.dwSize = sizeof(pe);

  if (! Process32FirstW(snapshot.get(), &pe)) {
    const unsigned long error = GetLastError();
    if (! k->traversal_error) k->traversal_error = error;
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_PROCESS_ENUMERATE_FAILED,
              k->name, error_string(error), 0);
    return;
  }

  /* This is a child of the doomed process so kill it. */
  k->depth++;
  if (! check_parent(k, &pe, pid)) {
    k->pid = pe.th32ProcessID;
    walk_process_tree(service, fn, k, pid);
  }
  k->pid = pid;

  while (true) {
    /* Try to get the next process. */
    if (! Process32NextW(snapshot.get(), &pe)) {
      unsigned long ret = GetLastError();
      if (ret == ERROR_NO_MORE_FILES) break;
      if (! k->traversal_error) k->traversal_error = ret;
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_PROCESS_ENUMERATE_FAILED,
                k->name, error_string(ret), 0);
      k->depth = depth;
      return;
    }

    if (! check_parent(k, &pe, pid)) {
      k->pid = pe.th32ProcessID;
      walk_process_tree(service, fn, k, pid);
    }
    k->pid = pid;
  }
  k->depth = depth;

}

void kill_process_tree(kill_t *k, unsigned long ppid) {
  return walk_process_tree(NULL, kill_process, k, ppid);
}
