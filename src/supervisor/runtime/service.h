#ifndef SERVICE_H
#define SERVICE_H

#include "windows_limits.h"

#include <windows.h>
#include <tchar.h>

/*
  MSDN says the commandline in CreateProcess() is limited to 32768 characters
  and the application name to MAX_PATH.
  A service name and service display name are limited to 256 characters.
  A registry key is limited to 255 characters.
  A registry value is limited to 16383 characters.
  Therefore we limit the service name to accommodate the path under HKLM.
*/
inline constexpr size_t EXE_LENGTH = PATH_LENGTH;

#define NSSM_ROTATE_OFFLINE 0
#define NSSM_ROTATE_ONLINE 1
#define NSSM_ROTATE_ONLINE_ASAP 2

struct service_config_t;
struct recovery_state_t;
struct status_snapshot_t;

typedef struct nssm_service_t {
  TCHAR name[SERVICE_NAME_LENGTH];
  TCHAR displayname[SERVICE_NAME_LENGTH];
  service_config_t *config;
  recovery_state_t *recovery_state;
  TCHAR exe[EXE_LENGTH];
  TCHAR flags[VALUE_LENGTH];
  TCHAR dir[DIR_LENGTH];
  TCHAR *env;
  unsigned long envlen;
  TCHAR *env_extra;
  unsigned long env_extralen;
  unsigned long no_console;
  TCHAR stdin_path[PATH_LENGTH];
  unsigned long stdin_sharing;
  unsigned long stdin_disposition;
  unsigned long stdin_flags;
  TCHAR stdout_path[PATH_LENGTH];
  unsigned long stdout_sharing;
  unsigned long stdout_disposition;
  unsigned long stdout_flags;
  bool use_stdout_pipe;
  HANDLE stdout_si;
  HANDLE stdout_pipe;
  HANDLE stdout_thread;
  unsigned long stdout_tid;
  TCHAR stderr_path[PATH_LENGTH];
  unsigned long stderr_sharing;
  unsigned long stderr_disposition;
  unsigned long stderr_flags;
  bool use_stderr_pipe;
  HANDLE stderr_si;
  HANDLE stderr_pipe;
  HANDLE stderr_thread;
  unsigned long stderr_tid;
  bool rotate_files;
  bool timestamp_log;
  bool stdout_copy_and_truncate;
  bool stderr_copy_and_truncate;
  unsigned long rotate_stdout_online;
  unsigned long rotate_stderr_online;
  unsigned long rotate_seconds;
  unsigned long rotate_bytes_low;
  unsigned long rotate_bytes_high;
  unsigned long rotate_delay;
  unsigned long kill_console_delay;
  unsigned long kill_window_delay;
  unsigned long kill_threads_delay;
  bool kill_process_tree;
  SERVICE_STATUS status;
  SRWLOCK status_lock;
  SERVICE_STATUS_HANDLE status_handle;
  HANDLE process_handle;
  HANDLE job_handle;
  unsigned long pid;
  HANDLE wait_handle;
  SRWLOCK wait_lock;
  HANDLE stop_event;
  unsigned long exitcode;
  bool stopping;
  bool allow_restart;
  unsigned long throttle;
  CRITICAL_SECTION throttle_section;
  bool throttle_section_initialised;
  CRITICAL_SECTION health_section;
  bool health_section_initialised;
  HANDLE health_stop_event;
  HANDLE health_thread;
  unsigned long health_failures;
  unsigned long health_successes;
  bool health_degraded;
  CRITICAL_SECTION resource_section;
  bool resource_section_initialised;
  HANDLE resource_stop_event;
  HANDLE resource_thread;
  unsigned long resource_cpu_violations;
  unsigned long resource_cpu_percent;
  unsigned long resource_processes;
  unsigned long long resource_memory_bytes;
  HANDLE status_mapping;
  status_snapshot_t *status_snapshot;
  SRWLOCK status_snapshot_lock;
  CONDITION_VARIABLE throttle_condition;
  FILETIME nssm_creation_time;
  FILETIME creation_time;
  FILETIME exit_time;
  TCHAR *initial_env;
  unsigned long next_restart_delay_ms;
  unsigned long long process_started_ms;
} nssm_service_t;

void WINAPI service_main(unsigned long, TCHAR **);
unsigned long WINAPI service_control_handler(unsigned long, unsigned long, void *, void *);

nssm_service_t *alloc_nssm_service();
void set_nssm_service_defaults(nssm_service_t *);
void cleanup_nssm_service(nssm_service_t *);
int monitor_service(nssm_service_t *);
int start_service(nssm_service_t *);
int stop_service(nssm_service_t *, unsigned long);
void CALLBACK end_service(void *, unsigned char);
void throttle_restart(nssm_service_t *);

#endif
