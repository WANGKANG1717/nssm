#ifndef PROCESS_H
#define PROCESS_H

typedef struct nssm_service_t nssm_service_t;

#include <windows.h>
#include <tlhelp32.h>

typedef struct {
  const wchar_t *name;
  HANDLE process_handle;
  unsigned long depth;
  unsigned long pid;
  unsigned long exitcode;
  unsigned long kill_console_delay;
  unsigned long kill_window_delay;
  unsigned long kill_threads_delay;
  SERVICE_STATUS_HANDLE status_handle;
  SERVICE_STATUS *status;
  SRWLOCK *status_lock;
  FILETIME creation_time;
  FILETIME exit_time;
  int signalled;
  unsigned long traversal_error;
  void *user_data;
} kill_t;

typedef int (*walk_function_t)(nssm_service_t *, kill_t *);

void service_kill_t(nssm_service_t *, kill_t *);
int get_process_creation_time(HANDLE, FILETIME *);
int get_process_exit_time(HANDLE, FILETIME *);
int check_parent(kill_t *, PROCESSENTRY32W *, unsigned long);
int CALLBACK kill_window(HWND, LPARAM);
int kill_threads(nssm_service_t *, kill_t *);
int kill_threads(kill_t *);
int kill_console(nssm_service_t *, kill_t *);
int kill_console(kill_t *);
int kill_process(nssm_service_t *, kill_t *);
int kill_process(kill_t *);
void walk_process_tree(nssm_service_t *, walk_function_t, kill_t *, unsigned long);
void kill_process_tree(kill_t *, unsigned long);

#endif
