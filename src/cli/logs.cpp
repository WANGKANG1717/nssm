#include "logs.h"

#include "config.h"
#include "config_registration.h"
#include "console.h"
#include "event.h"
#include "output.h"
#include "utf8.h"
#include "win32_handle.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {
struct log_source_t { std::wstring label; std::wstring path; };
HANDLE follow_stop_event = nullptr;

bool read_chunk(HANDLE file, unsigned long long offset, std::vector<char> *data,
                unsigned long *error) {
  LARGE_INTEGER position = {}; position.QuadPart = offset;
  if (! SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
    *error = GetLastError(); return false;
  }
  LARGE_INTEGER size = {};
  if (! GetFileSizeEx(file, &size)) { *error = GetLastError(); return false; }
  if (size.QuadPart <= static_cast<long long>(offset)) { data->clear(); return true; }
  const unsigned long long available = size.QuadPart - offset;
  data->resize(static_cast<size_t>(std::min<unsigned long long>(available, 1024 * 1024)));
  unsigned long read = 0;
  if (! ReadFile(file, data->data(), static_cast<unsigned long>(data->size()),
                 &read, nullptr)) { *error = GetLastError(); return false; }
  data->resize(read); return true;
}

bool write_log_bytes(const char *data, size_t size) {
  unsigned long error = ERROR_SUCCESS;
  if (write_standard_output_bytes(data, size, &error)) return true;
  fwprintf(stderr, L"unable to write log output: %ls\n", error_string(error));
  return false;
}

bool print_tail(const log_source_t &source, unsigned long lines, bool header,
                bool follow, HANDLE stop_event) {
  unique_handle file(CreateFileW(source.path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (! file) { fwprintf(stderr, L"%ls: %ls\n", source.path.c_str(), error_string(GetLastError())); return false; }
  LARGE_INTEGER size = {}; GetFileSizeEx(file.get(), &size);
  const unsigned long long window = 4ULL * 1024ULL * 1024ULL;
  const unsigned long long start = size.QuadPart > static_cast<long long>(window)
                                       ? size.QuadPart - window : 0;
  std::vector<char> data; unsigned long error = ERROR_SUCCESS;
  read_chunk(file.get(), start, &data, &error);
  size_t begin = data.size(); unsigned long found = 0;
  while (begin && found <= lines) if (data[--begin] == '\n') ++found;
  if (found > lines) ++begin; else begin = 0;
  if (header) fwprintf(stdout, L"==> %ls (%ls) <==\n", source.label.c_str(), source.path.c_str());
  if (begin < data.size() &&
      ! write_log_bytes(data.data() + begin, data.size() - begin)) return false;
  unsigned long long offset = static_cast<unsigned long long>(size.QuadPart);
  while (follow && WaitForSingleObject(stop_event, 250) == WAIT_TIMEOUT) {
    if (! read_chunk(file.get(), offset, &data, &error)) break;
    if (! data.empty()) {
      if (! write_log_bytes(data.data(), data.size())) return false;
      offset += data.size();
    }
    LARGE_INTEGER current = {}; if (GetFileSizeEx(file.get(), &current) && current.QuadPart < static_cast<long long>(offset)) offset = 0;
  }
  return true;
}

BOOL WINAPI stop_follow(unsigned long control) {
  if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT) {
    if (follow_stop_event) SetEvent(follow_stop_event);
    return TRUE;
  }
  return FALSE;
}
}

int logs_command(int argc, wchar_t **argv) {
  bool follow = false; unsigned long lines = 100; std::wstring service;
  for (int i = 0; i < argc; ++i) {
    if (! wcscmp(argv[i], L"--follow") || ! wcscmp(argv[i], L"-f")) follow = true;
    else if ((! wcscmp(argv[i], L"--lines") ||
              ! wcscmp(argv[i], L"-n")) && i + 1 < argc) {
      wchar_t *end = nullptr; unsigned long value = wcstoul(argv[++i], &end, 10);
      if (! end || *end || value > 100000) return 2;
      lines = value;
    }
    else if (argv[i][0] == L'-' || ! service.empty()) return 2;
    else service = argv[i];
  }
  if (service.empty()) { fwprintf(stderr, L"Usage: svp logs [-n N|--lines N] [-f|--follow] <service>\n"); return 2; }
  std::wstring config_path; unsigned long error = ERROR_SUCCESS;
  service_config_t config; config_validation_t validation;
  if (! read_config_path(service, &config_path, &error) ||
      ! load_service_config(config_path.c_str(), &config, &validation)) {
    fwprintf(stderr, L"%ls: unable to load logging configuration\n", service.c_str()); return 5;
  }
  std::vector<log_source_t> sources;
  if (! config.logging.stdout_path.empty()) sources.push_back({L"stdout", config.logging.stdout_path});
  if (! config.logging.stderr_path.empty() && config.logging.stderr_path != config.logging.stdout_path) sources.push_back({L"stderr", config.logging.stderr_path});
  if (sources.empty()) { fwprintf(stderr, L"%ls: logging is not configured\n", service.c_str()); return 4; }
  unique_handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  for (const auto &source : sources) {
    if (! print_tail(source, lines, sources.size() > 1, false, stop.get())) return 5;
  }
  if (follow) {
    if (sources.size() > 1) fwprintf(stderr, L"--follow requires stdout and stderr to share one path\n");
    else {
      follow_stop_event = stop.get();
      SetConsoleCtrlHandler(stop_follow, TRUE);
      const bool followed = print_tail(sources.front(), 0, false, true, stop.get());
      SetConsoleCtrlHandler(stop_follow, FALSE);
      follow_stop_event = nullptr;
      if (! followed) return 5;
    }
  }
  return follow && sources.size() > 1 ? 2 : 0;
}
