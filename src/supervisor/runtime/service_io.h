#ifndef SVP_IO_H
#define SVP_IO_H

#include <windows.h>

struct service_runtime_t;

#define SVP_STDIN_SHARING FILE_SHARE_WRITE
#define SVP_STDIN_DISPOSITION OPEN_EXISTING
#define SVP_STDIN_FLAGS FILE_ATTRIBUTE_NORMAL
#define SVP_STDOUT_SHARING (FILE_SHARE_READ | FILE_SHARE_WRITE)
#define SVP_STDOUT_DISPOSITION OPEN_ALWAYS
#define SVP_STDOUT_FLAGS FILE_ATTRIBUTE_NORMAL
#define SVP_STDERR_SHARING (FILE_SHARE_READ | FILE_SHARE_WRITE)
#define SVP_STDERR_DISPOSITION OPEN_ALWAYS
#define SVP_STDERR_FLAGS FILE_ATTRIBUTE_NORMAL

typedef struct {
  wchar_t *service_name;
  wchar_t *path;
  unsigned long sharing;
  unsigned long disposition;
  unsigned long flags;
  HANDLE read_handle;
  HANDLE write_handle;
  __int64 size;
  unsigned long *tid_ptr;
  volatile LONG *rotate_online;
  HANDLE stop_event;
  bool timestamp_log;
  __int64 line_length;
  bool copy_and_truncate;
  unsigned long rotate_delay;
} logger_t;

void close_handle(HANDLE *);
HANDLE write_to_file(wchar_t *, unsigned long, SECURITY_ATTRIBUTES *, unsigned long, unsigned long);
void rotate_file(wchar_t *, wchar_t *, unsigned long, unsigned long, unsigned long, unsigned long, bool);
int get_output_handles(service_runtime_t *, STARTUPINFOW *);
void close_output_handles(STARTUPINFOW *);
void cleanup_loggers(service_runtime_t *);
unsigned long WINAPI log_and_rotate(void *);
LONG logger_rotation_state(volatile LONG *);
void set_logger_rotation_state(volatile LONG *, LONG);
bool request_logger_rotation(volatile LONG *);
bool claim_logger_rotation(volatile LONG *, bool);
void finish_logger_rotation(volatile LONG *);

#endif
