#include "console.h"
#include "event.h"
#include "messages.h"
#include "service.h"
#include "service_constants.h"
#include "string_utils.h"
#include "nssm_io.h"
#include "utf8.h"
#include "win32_handle.h"

#include <shlwapi.h>

#define COMPLAINED_READ (1 << 0)
#define COMPLAINED_WRITE (1 << 1)
#define COMPLAINED_ROTATE (1 << 2)
#define TIMESTAMP_FORMAT "%04u-%02u-%02u %02u:%02u:%02u.%03u: "
#define TIMESTAMP_LEN 25

static int dup_handle(HANDLE source_handle, HANDLE *dest_handle_ptr, TCHAR *source_description, TCHAR *dest_description, unsigned long flags) {
  if (! dest_handle_ptr) return 1;

  if (! DuplicateHandle(GetCurrentProcess(), source_handle, GetCurrentProcess(), dest_handle_ptr, 0, true, flags)) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_DUPLICATEHANDLE_FAILED, source_description, dest_description, error_string(GetLastError()), 0);
    return 2;
  }
  return 0;
}

static int dup_handle(HANDLE source_handle, HANDLE *dest_handle_ptr, TCHAR *source_description, TCHAR *dest_description) {
  return dup_handle(source_handle, dest_handle_ptr, source_description, dest_description, DUPLICATE_SAME_ACCESS);
}

/*
  read_handle:  read from application
  pipe_handle:  stdout of application
  write_handle: to file
*/
static HANDLE create_logging_thread(TCHAR *service_name, TCHAR *path, unsigned long sharing, unsigned long disposition, unsigned long flags, HANDLE *read_handle_ptr, HANDLE *pipe_handle_ptr, HANDLE *write_handle_ptr, unsigned long rotate_bytes_low, unsigned long rotate_bytes_high, unsigned long rotate_delay, unsigned long *tid_ptr, unsigned long *rotate_online, bool timestamp_log, bool copy_and_truncate) {
  *tid_ptr = 0;

  /* Pipe between application's stdout/stderr and our logging handle. */
  if (read_handle_ptr && ! *read_handle_ptr) {
    if (pipe_handle_ptr && ! *pipe_handle_ptr) {
      if (CreatePipe(read_handle_ptr, pipe_handle_ptr, 0, 0)) {
        if (! SetHandleInformation(*pipe_handle_ptr, HANDLE_FLAG_INHERIT,
                                   HANDLE_FLAG_INHERIT)) {
          const unsigned long error = GetLastError();
          CloseHandle(*read_handle_ptr);
          CloseHandle(*pipe_handle_ptr);
          *read_handle_ptr = nullptr;
          *pipe_handle_ptr = nullptr;
          log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEPIPE_FAILED,
                    service_name, path, error_string(error), 0);
          SetLastError(error);
          return nullptr;
        }
      }
      else {
        log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEPIPE_FAILED,
                  service_name, path, error_string(GetLastError()), 0);
        return (HANDLE) 0;
      }
    }
  }

  logger_t *logger = (logger_t *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(logger_t));
  if (! logger) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("logger"), _T("create_logging_thread()"), 0);
    return (HANDLE) 0;
  }

  ULARGE_INTEGER size;
  size.LowPart = rotate_bytes_low;
  size.HighPart = rotate_bytes_high;

  logger->service_name = service_name;
  logger->path = path;
  logger->sharing = sharing;
  logger->disposition = disposition;
  logger->flags = flags;
  logger->read_handle = *read_handle_ptr;
  logger->write_handle = *write_handle_ptr;
  logger->size = (__int64) size.QuadPart;
  logger->tid_ptr = tid_ptr;
  logger->timestamp_log = timestamp_log;
  logger->line_length = 0;
  logger->rotate_online = rotate_online;
  logger->rotate_delay = rotate_delay;
  logger->copy_and_truncate = copy_and_truncate;

  HANDLE thread_handle = CreateThread(NULL, 0, log_and_rotate, (void *) logger, 0, logger->tid_ptr);
  if (! thread_handle) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATETHREAD_FAILED, error_string(GetLastError()), 0);
    HeapFree(GetProcessHeap(), 0, logger);
  }

  return thread_handle;
}

static inline unsigned long guess_charsize(void *address, unsigned long bufsize) {
  if (IsTextUnicode(address, bufsize, 0)) return (unsigned long) sizeof(wchar_t);
  else return (unsigned long) sizeof(char);
}

static inline void write_bom(logger_t *logger, unsigned long *out) {
  *out = 0;
  wchar_t bom = L'\ufeff';
  if (! WriteFile(logger->write_handle, (void *) &bom, sizeof(bom), out, 0)) {
    log_event(EVENTLOG_WARNING_TYPE, NSSM_EVENT_SOMEBODY_SET_UP_US_THE_BOM, logger->service_name, logger->path, error_string(GetLastError()), 0);
  }
}

void close_handle(HANDLE *handle) {
  if (! handle) return;
  if (! *handle) return;
  CloseHandle(*handle);
  *handle = 0;
}

HANDLE write_to_file(TCHAR *path, unsigned long sharing, SECURITY_ATTRIBUTES *attributes, unsigned long disposition, unsigned long flags) {
  const LARGE_INTEGER offset = {};
  unique_handle file(CreateFile(path, FILE_WRITE_DATA, sharing, attributes,
                                disposition, flags, nullptr));
  if (file && SetFilePointerEx(file.get(), offset, nullptr, FILE_END) &&
      SetEndOfFile(file.get())) {
    return file.release();
  }

  const unsigned long error = GetLastError();
  file.reset();
  log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEFILE_FAILED, path,
            error_string(error), 0);
  SetLastError(error);
  return INVALID_HANDLE_VALUE;
}

static void rotated_filename(TCHAR *path, TCHAR *rotated, unsigned long rotated_len, SYSTEMTIME *st) {
  SYSTEMTIME now{};
  if (! st) {
    GetSystemTime(&now);
    st = &now;
  }

  TCHAR buffer[PATH_LENGTH];
  memmove(buffer, path, sizeof(buffer));
  TCHAR *ext = PathFindExtension(buffer);
  TCHAR extension[PATH_LENGTH];
  _sntprintf_s(extension, _countof(extension), _TRUNCATE, _T("-%04u%02u%02uT%02u%02u%02u.%03u%s"), st->wYear, st->wMonth, st->wDay, st->wHour, st->wMinute, st->wSecond, st->wMilliseconds, ext);
  *ext = _T('\0');
  _sntprintf_s(rotated, rotated_len, _TRUNCATE, _T("%s%s"), buffer, extension);
}

void rotate_file(TCHAR *service_name, TCHAR *path, unsigned long seconds, unsigned long delay, unsigned long low, unsigned long high, bool copy_and_truncate) {
  unsigned long error;

  /* Now. */
  SYSTEMTIME st;
  GetSystemTime(&st);

  BY_HANDLE_FILE_INFORMATION info = {};

  /* Try to open the file to check if it exists and to get attributes. */
  HANDLE file = CreateFile(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if (file != INVALID_HANDLE_VALUE) {
    /* Get file attributes. */
    if (! GetFileInformationByHandle(file, &info)) {
      /* Reuse current time for rotation timestamp. */
      seconds = low = high = 0;
      SystemTimeToFileTime(&st, &info.ftLastWriteTime);
    }

    CloseHandle(file);
  }
  else {
    error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return;
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_ROTATE_FILE_FAILED, service_name, path, _T("CreateFile()"), path, error_string(error), 0);
    /* Reuse current time for rotation timestamp. */
    seconds = low = high = 0;
    SystemTimeToFileTime(&st, &info.ftLastWriteTime);
  }

  /* Check file age. */
  if (seconds) {
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);

    ULARGE_INTEGER s;
    s.LowPart = ft.dwLowDateTime;
    s.HighPart = ft.dwHighDateTime;
    s.QuadPart -= seconds * 10000000LL;
    ft.dwLowDateTime = s.LowPart;
    ft.dwHighDateTime = s.HighPart;
    if (CompareFileTime(&info.ftLastWriteTime, &ft) > 0) return;
  }

  /* Check file size. */
  if (low || high) {
    if (info.nFileSizeHigh < high) return;
    if (info.nFileSizeHigh == high && info.nFileSizeLow < low) return;
  }

  /* Get new filename. */
  FileTimeToSystemTime(&info.ftLastWriteTime, &st);

  TCHAR rotated[PATH_LENGTH];
  rotated_filename(path, rotated, _countof(rotated), &st);

  /* Rotate. */
  bool ok = true;
  TCHAR *function;
  if (copy_and_truncate) {
    function = _T("CopyFile()");
    if (CopyFile(path, rotated, TRUE)) {
      file = write_to_file(path, NSSM_STDOUT_SHARING, 0, NSSM_STDOUT_DISPOSITION, NSSM_STDOUT_FLAGS);
      if (file == INVALID_HANDLE_VALUE) ok = false;
      else {
        Sleep(delay);
        SetFilePointer(file, 0, 0, FILE_BEGIN);
        SetEndOfFile(file);
        CloseHandle(file);
      }
    }
    else ok = false;
  }
  else {
    function = _T("MoveFile()");
    if (! MoveFile(path, rotated)) ok = false;
  }
  if (ok) {
    log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_ROTATED, service_name, path, rotated, 0);
    return;
  }
  error = GetLastError();

  if (error == ERROR_FILE_NOT_FOUND) return;
  log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_ROTATE_FILE_FAILED, service_name, path, function, rotated, error_string(error), 0);
  return;
}

int get_output_handles(nssm_service_t *service, STARTUPINFO *si) {
  if (! si) return 1;
  bool inherit_handles = false;

  /* Allocate a new console so we get a fresh stdin, stdout and stderr. */
  allocate_console(! service->no_console);

  /* stdin */
  if (service->stdin_path[0]) {
    si->hStdInput = CreateFile(service->stdin_path, FILE_READ_DATA, service->stdin_sharing, 0, service->stdin_disposition, service->stdin_flags, 0);
    if (si->hStdInput == INVALID_HANDLE_VALUE) {
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEFILE_FAILED, service->stdin_path, error_string(GetLastError()), 0);
      return 2;
    }

    inherit_handles = true;
  }

  /* stdout */
  if (service->stdout_path[0]) {
    if (service->rotate_files) {
      rotate_file(service->name, service->stdout_path,
                  service->rotate_seconds, service->rotate_delay,
                  service->rotate_bytes_low, service->rotate_bytes_high,
                  service->stdout_copy_and_truncate);
    }
    HANDLE stdout_handle = write_to_file(service->stdout_path, service->stdout_sharing, 0, service->stdout_disposition, service->stdout_flags);
    if (stdout_handle == INVALID_HANDLE_VALUE) return 4;
    service->stdout_si = 0;

    if (service->use_stdout_pipe) {
      service->stdout_pipe = si->hStdOutput = 0;
      service->stdout_thread = create_logging_thread(service->name, service->stdout_path, service->stdout_sharing, service->stdout_disposition, service->stdout_flags, &service->stdout_pipe, &service->stdout_si, &stdout_handle, service->rotate_bytes_low, service->rotate_bytes_high, service->rotate_delay, &service->stdout_tid, &service->rotate_stdout_online, service->timestamp_log, service->stdout_copy_and_truncate);
      if (! service->stdout_thread) {
        CloseHandle(service->stdout_pipe);
        CloseHandle(service->stdout_si);
        service->stdout_pipe = nullptr;
        service->stdout_si = nullptr;
      }
    }
    else service->stdout_thread = 0;

    if (! service->stdout_thread) {
      if (dup_handle(stdout_handle, &service->stdout_si, _T("stdout file"),
                     _T("stdout"),
                     DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) return 4;
      service->rotate_stdout_online = NSSM_ROTATE_OFFLINE;
    }

    if (dup_handle(service->stdout_si, &si->hStdOutput, _T("stdout_si"),
                   _T("stdout"))) {
      cleanup_loggers(service);
      return 5;
    }

    inherit_handles = true;
  }

  /* stderr */
  if (service->stderr_path[0]) {
    /* Same as stdout? */
    if (strings_equal_ignore_case(service->stderr_path,
                                  service->stdout_path)) {
      service->stderr_sharing = service->stdout_sharing;
      service->stderr_disposition = service->stdout_disposition;
      service->stderr_flags = service->stdout_flags;
      service->rotate_stderr_online = NSSM_ROTATE_OFFLINE;

      /* Two handles to the same file will create a race. */
      /* XXX: Here we assume that either both or neither handle must be a pipe. */
      if (dup_handle(service->stdout_si, &service->stderr_si, _T("stdout"), _T("stderr"))) return 6;
    }
    else {
      if (service->rotate_files) {
        rotate_file(service->name, service->stderr_path,
                    service->rotate_seconds, service->rotate_delay,
                    service->rotate_bytes_low, service->rotate_bytes_high,
                    service->stderr_copy_and_truncate);
      }
      HANDLE stderr_handle = write_to_file(service->stderr_path, service->stderr_sharing, 0, service->stderr_disposition, service->stderr_flags);
      if (stderr_handle == INVALID_HANDLE_VALUE) return 7;
      service->stderr_si = 0;

      if (service->use_stderr_pipe) {
        service->stderr_pipe = si->hStdError = 0;
        service->stderr_thread = create_logging_thread(service->name, service->stderr_path, service->stderr_sharing, service->stderr_disposition, service->stderr_flags, &service->stderr_pipe, &service->stderr_si, &stderr_handle, service->rotate_bytes_low, service->rotate_bytes_high, service->rotate_delay, &service->stderr_tid, &service->rotate_stderr_online, service->timestamp_log, service->stderr_copy_and_truncate);
        if (! service->stderr_thread) {
          CloseHandle(service->stderr_pipe);
          CloseHandle(service->stderr_si);
          service->stderr_pipe = nullptr;
          service->stderr_si = nullptr;
        }
      }
      else service->stderr_thread = 0;

      if (! service->stderr_thread) {
        if (dup_handle(stderr_handle, &service->stderr_si,
                       _T("stderr file"), _T("stderr"),
                       DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) return 7;
        service->rotate_stderr_online = NSSM_ROTATE_OFFLINE;
      }
    }

    if (dup_handle(service->stderr_si, &si->hStdError, _T("stderr_si"),
                   _T("stderr"))) {
      cleanup_loggers(service);
      return 8;
    }

    inherit_handles = true;
  }

  /*
    We need to set the startup_info flags to make the new handles
    inheritable by the new process.
  */
  if (inherit_handles) si->dwFlags |= STARTF_USESTDHANDLES;

  return 0;
}

void close_output_handles(STARTUPINFO *si) {
  if (si->hStdInput) CloseHandle(si->hStdInput);
  if (si->hStdOutput) CloseHandle(si->hStdOutput);
  if (si->hStdError) CloseHandle(si->hStdError);
}

void cleanup_loggers(nssm_service_t *service) {
  unsigned long interval = kLoggerCleanupDeadlineMs;
  HANDLE thread_handle = service->stdout_thread;
  service->stdout_thread = nullptr;
  /* Close write end of the data pipe so logging thread can finalise read. */
  close_handle(&service->stdout_si);
  if (thread_handle) {
    if (WaitForSingleObject(thread_handle, interval) == WAIT_TIMEOUT) {
      CancelSynchronousIo(thread_handle);
      WaitForSingleObject(thread_handle, INFINITE);
    }
    CloseHandle(thread_handle);
    /* The logging thread owns and closes the read end. */
    service->stdout_pipe = nullptr;
  }
  else close_handle(&service->stdout_pipe);

  thread_handle = service->stderr_thread;
  service->stderr_thread = nullptr;
  close_handle(&service->stderr_si);
  if (thread_handle) {
    if (WaitForSingleObject(thread_handle, interval) == WAIT_TIMEOUT) {
      CancelSynchronousIo(thread_handle);
      WaitForSingleObject(thread_handle, INFINITE);
    }
    CloseHandle(thread_handle);
    service->stderr_pipe = nullptr;
  }
  else close_handle(&service->stderr_pipe);
}

/*
  Try multiple times to read from a file.
  Returns:  0 on success.
            1 on non-fatal error.
           -1 on fatal error.
*/
static int try_read(logger_t *logger, void *address, unsigned long bufsize, unsigned long *in, int *complained) {
  int ret = 1;
  unsigned long error;
  for (int tries = 0; tries < 5; tries++) {
    if (ReadFile(logger->read_handle, address, bufsize, in, 0)) return 0;

    error = GetLastError();
    switch (error) {
      /* Other end closed the pipe. */
      case ERROR_BROKEN_PIPE:
        ret = -1;
        goto complain_read;

      /* Couldn't lock the buffer. */
      case ERROR_NOT_ENOUGH_QUOTA:
        Sleep(2000 + tries * 3000);
        ret = 1;
        continue;

      /* Write was cancelled by the other end. */
      case ERROR_OPERATION_ABORTED:
        ret = -1;
        goto complain_read;

      default:
        ret = -1;
    }
  }

complain_read:
  /* Ignore the error if we've been requested to exit anyway. */
  if (*logger->rotate_online != NSSM_ROTATE_ONLINE) return ret;
  if (! (*complained & COMPLAINED_READ)) log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_READFILE_FAILED, logger->service_name, logger->path, error_string(error), 0);
  *complained |= COMPLAINED_READ;
  return ret;
}

/*
  Try multiple times to write to a file.
  Returns:  0 on success.
            1 on non-fatal error.
           -1 on fatal error.
*/
static int try_write(logger_t *logger, void *address, unsigned long bufsize, unsigned long *out, int *complained) {
  *out = 0;
  int ret = 1;
  unsigned long error;
  for (int tries = 0; tries < 5; tries++) {
    if (WriteFile(logger->write_handle, address, bufsize, out, 0)) return 0;

    error = GetLastError();
    if (error == ERROR_IO_PENDING) {
      /* Operation was successful pending flush to disk. */
      return 0;
    }

    switch (error) {
      /* Other end closed the pipe. */
      case ERROR_BROKEN_PIPE:
        ret = -1;
        goto complain_write;

      /* Couldn't lock the buffer. */
      case ERROR_NOT_ENOUGH_QUOTA:
      /* Out of disk space. */
      case ERROR_DISK_FULL:
        Sleep(2000 + tries * 3000);
        ret = 1;
        continue;

      default:
        /* We'll lose this line but try to read and write subsequent ones. */
        ret = 1;
    }
  }

complain_write:
  if (! (*complained & COMPLAINED_WRITE)) log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_WRITEFILE_FAILED, logger->service_name, logger->path, error_string(error), 0);
  *complained |= COMPLAINED_WRITE;
  return ret;
}

/* Note that the timestamp is created in UTF-8. */
static inline int write_timestamp(logger_t *logger, unsigned long charsize, unsigned long *out, int *complained) {
  char timestamp[TIMESTAMP_LEN + 1];

  SYSTEMTIME now;
  GetSystemTime(&now);
  _snprintf_s(timestamp, _countof(timestamp), _TRUNCATE, TIMESTAMP_FORMAT, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

  if (charsize == sizeof(char)) return try_write(logger, (void *) timestamp, TIMESTAMP_LEN, out, complained);

  wchar_t *utf16;
  unsigned long utf16len;
  if (to_utf16(timestamp, &utf16, &utf16len)) return -1;
  int ret = try_write(logger, utf16, utf16len * sizeof(wchar_t), out, complained);
  HeapFree(GetProcessHeap(), 0, utf16);
  return ret;
}

static int write_with_timestamp(logger_t *logger, void *address, unsigned long bufsize, unsigned long *out, int *complained, unsigned long charsize) {
  *out = 0;
  if (logger->timestamp_log) {
    unsigned long log_out = 0;
    int log_complained = 0;
    unsigned long timestamp_out = 0;
    int timestamp_complained = 0;
    if (! logger->line_length) {
      write_timestamp(logger, charsize, &timestamp_out, &timestamp_complained);
      logger->line_length += (__int64) timestamp_out;
      *out += timestamp_out;
      *complained |= timestamp_complained;
    }

    unsigned long i;
    void *line = address;
    unsigned long offset = 0;
    int ret = 0;
    for (i = 0; i < bufsize; i++) {
      if (((char *) address)[i] == '\n') {
        ret = try_write(logger, line, i - offset + 1, &log_out, &log_complained);
        line = (void *) ((char *) line + i - offset + 1);
        logger->line_length = 0LL;
        *out += log_out;
        *complained |= log_complained;
        offset = i + 1;
        if (offset < bufsize) {
          write_timestamp(logger, charsize, &timestamp_out, &timestamp_complained);
          logger->line_length += (__int64) timestamp_out;
          *out += timestamp_out;
          *complained |= timestamp_complained;
        }
      }
    }

    if (offset < bufsize) {
      ret = try_write(logger, line, bufsize - offset, &log_out, &log_complained);
      *out += log_out;
      *complained |= log_complained;
    }

    return ret;
  }
  else return try_write(logger, address, bufsize, out, complained);
}

/* Wrapper to be called in a new thread for logging. */
unsigned long WINAPI log_and_rotate(void *arg) {
  logger_t *logger = (logger_t *) arg;
  if (! logger) return 1;

  __int64 size = 0;
  BY_HANDLE_FILE_INFORMATION info;

  /* Find initial file size. */
  if (! GetFileInformationByHandle(logger->write_handle, &info)) size = 0LL;
  else {
    ULARGE_INTEGER l;
    l.HighPart = info.nFileSizeHigh;
    l.LowPart = info.nFileSizeLow;
    size = l.QuadPart;
  }

  char buffer[1024];
  void *address;
  unsigned long in, out;
  unsigned long charsize = 0;
  unsigned long error;
  int ret;
  int complained = 0;

  while (true) {
    /* Read data from the pipe. */
    in = 0;
    out = 0;
    address = &buffer;
    ret = try_read(logger, address, sizeof(buffer), &in, &complained);
    if (ret < 0) {
      close_handle(&logger->read_handle);
      close_handle(&logger->write_handle);
      HeapFree(GetProcessHeap(), 0, logger);
      return 2;
    }
    else if (ret) continue;

    if (*logger->rotate_online == NSSM_ROTATE_ONLINE_ASAP || (logger->size && size + (__int64) in >= logger->size)) {
      /* Look for newline. */
      unsigned long i;
      for (i = 0; i < in; i++) {
        if (buffer[i] == '\n') {
          if (! charsize) charsize = guess_charsize(address, in);
          i += charsize;

          /* Write up to the newline. */
          ret = try_write(logger, address, i, &out, &complained);
          if (ret < 0) {
            close_handle(&logger->read_handle);
            close_handle(&logger->write_handle);
            HeapFree(GetProcessHeap(), 0, logger);
            return 3;
          }
          size += (__int64) out;

          /* Rotate. */
          *logger->rotate_online = NSSM_ROTATE_ONLINE;
          TCHAR rotated[PATH_LENGTH];
          rotated_filename(logger->path, rotated, _countof(rotated), 0);

          /*
            Ideally we'd try the rename first then close the handle but
            MoveFile() will fail if the handle is still open so we must
            risk losing everything.
          */
          if (logger->copy_and_truncate) FlushFileBuffers(logger->write_handle);
          close_handle(&logger->write_handle);
          bool ok = true;
          TCHAR *function;
          if (logger->copy_and_truncate) {
            function = _T("CopyFile()");
            if (CopyFile(logger->path, rotated, TRUE)) {
              HANDLE file = write_to_file(logger->path, NSSM_STDOUT_SHARING, 0, NSSM_STDOUT_DISPOSITION, NSSM_STDOUT_FLAGS);
              if (file == INVALID_HANDLE_VALUE) ok = false;
              else {
                Sleep(logger->rotate_delay);
                SetFilePointer(file, 0, 0, FILE_BEGIN);
                SetEndOfFile(file);
                CloseHandle(file);
              }
            }
            else ok = false;
          }
          else {
            function = _T("MoveFile()");
            if (! MoveFile(logger->path, rotated)) ok = false;
          }
          if (ok) {
            log_event(EVENTLOG_INFORMATION_TYPE, NSSM_EVENT_ROTATED, logger->service_name, logger->path, rotated, 0);
            size = 0LL;
          }
          else {
            error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND) {
              if (! (complained & COMPLAINED_ROTATE)) log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_ROTATE_FILE_FAILED, logger->service_name, logger->path, function, rotated, error_string(error), 0);
              complained |= COMPLAINED_ROTATE;
              /* We can at least try to re-open the existing file. */
              logger->disposition = OPEN_ALWAYS;
            }
          }

          /* Reopen. */
          logger->write_handle = write_to_file(logger->path, logger->sharing, 0, logger->disposition, logger->flags);
          if (logger->write_handle == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_CREATEFILE_FAILED, logger->path, error_string(error), 0);
            /* Oh dear.  Now we can't log anything further. */
            close_handle(&logger->read_handle);
            close_handle(&logger->write_handle);
            HeapFree(GetProcessHeap(), 0, logger);
            return 4;
          }

          /* Resume writing after the newline. */
          address = (void *) ((char *) address + i);
          in -= i;
          break;
        }
      }
    }

    if (! size || logger->timestamp_log) if (! charsize) charsize = guess_charsize(address, in);
    if (! size) {
      /* Write a BOM to the new file. */
      if (charsize == sizeof(wchar_t)) write_bom(logger, &out);
      size += (__int64) out;
    }

    /* Write the data, if any. */
    if (! in) continue;

    ret = write_with_timestamp(logger, address, in, &out, &complained, charsize);
    size += (__int64) out;
    if (ret < 0) {
      close_handle(&logger->read_handle);
      close_handle(&logger->write_handle);
      HeapFree(GetProcessHeap(), 0, logger);
      return 3;
    }
  }

  close_handle(&logger->read_handle);
  close_handle(&logger->write_handle);
  HeapFree(GetProcessHeap(), 0, logger);
  return 0;
}
