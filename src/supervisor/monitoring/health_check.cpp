#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>

#include "health_check.h"
#include "child_process.h"
#include "job_object.h"
#include "win32_handle.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using unique_winhttp_handle =
    unique_win32_resource<HINTERNET, WinHttpCloseHandle>;

health_probe_result_t failure(unsigned long error, std::wstring detail) {
  return {false, error, std::move(detail)};
}

health_probe_result_t check_process(HANDLE process) {
  if (! process) return failure(ERROR_INVALID_HANDLE, L"application process handle is unavailable");
  unsigned long exit_code = 0;
  if (! GetExitCodeProcess(process, &exit_code)) {
    return failure(GetLastError(), L"GetExitCodeProcess failed");
  }
  if (exit_code != STILL_ACTIVE) {
    return failure(ERROR_PROCESS_ABORTED,
                   L"application process exited with code " +
                       std::to_wstring(exit_code));
  }
  return {true, ERROR_SUCCESS, L"process is running"};
}

health_probe_result_t check_tcp(const health_check_config_t &config) {
  WSADATA data = {};
  int result = WSAStartup(MAKEWORD(2, 2), &data);
  if (result) return failure(static_cast<unsigned long>(result), L"WSAStartup failed");

  ADDRINFOW hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  ADDRINFOW *addresses = nullptr;
  std::wstring port = std::to_wstring(config.port);
  result = GetAddrInfoW(config.host.c_str(), port.c_str(), &hints, &addresses);
  if (result) {
    WSACleanup();
    return failure(static_cast<unsigned long>(result), L"DNS resolution failed");
  }

  unsigned long last_error = WSAETIMEDOUT;
  unsigned long long deadline = GetTickCount64() + config.timeout_ms;
  for (ADDRINFOW *address = addresses; address; address = address->ai_next) {
    SOCKET socket_handle = WSASocketW(address->ai_family, address->ai_socktype,
                                      address->ai_protocol, nullptr, 0, 0);
    if (socket_handle == INVALID_SOCKET) {
      last_error = WSAGetLastError();
      continue;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking)) {
      last_error = WSAGetLastError();
      closesocket(socket_handle);
      continue;
    }
    result = connect(socket_handle, address->ai_addr,
                     static_cast<int>(address->ai_addrlen));
    if (! result) {
      closesocket(socket_handle);
      FreeAddrInfoW(addresses);
      WSACleanup();
      return {true, ERROR_SUCCESS, L"TCP connection succeeded"};
    }
    last_error = WSAGetLastError();
    if (last_error == WSAEWOULDBLOCK || last_error == WSAEINPROGRESS) {
      unsigned long long now = GetTickCount64();
      unsigned long remaining = now < deadline
                                    ? static_cast<unsigned long>(deadline - now)
                                    : 0;
      fd_set writable;
      FD_ZERO(&writable);
      FD_SET(socket_handle, &writable);
      timeval timeout = {static_cast<long>(remaining / 1000),
                         static_cast<long>((remaining % 1000) * 1000)};
      result = select(0, nullptr, &writable, nullptr, &timeout);
      if (result > 0) {
        int socket_error = 0;
        int length = sizeof(socket_error);
        if (! getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char *>(&socket_error), &length) &&
            ! socket_error) {
          closesocket(socket_handle);
          FreeAddrInfoW(addresses);
          WSACleanup();
          return {true, ERROR_SUCCESS, L"TCP connection succeeded"};
        }
        last_error = socket_error ? static_cast<unsigned long>(socket_error)
                                  : static_cast<unsigned long>(WSAGetLastError());
      }
      else last_error = result == 0 ? WSAETIMEDOUT : WSAGetLastError();
    }
    closesocket(socket_handle);
    if (GetTickCount64() >= deadline) break;
  }
  FreeAddrInfoW(addresses);
  WSACleanup();
  return failure(last_error, L"TCP connection failed");
}

health_probe_result_t check_http(const health_check_config_t &config) {
  URL_COMPONENTSW components = {};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<unsigned long>(-1);
  components.dwHostNameLength = static_cast<unsigned long>(-1);
  components.dwUrlPathLength = static_cast<unsigned long>(-1);
  components.dwExtraInfoLength = static_cast<unsigned long>(-1);
  if (! WinHttpCrackUrl(config.url.c_str(), 0, 0, &components)) {
    return failure(GetLastError(), L"WinHttpCrackUrl failed");
  }
  std::wstring host(components.lpszHostName, components.dwHostNameLength);
  std::wstring target;
  if (components.dwUrlPathLength) {
    target.assign(components.lpszUrlPath, components.dwUrlPathLength);
  }
  if (components.dwExtraInfoLength) {
    target.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (target.empty()) target = L"/";

  unique_winhttp_handle session(WinHttpOpen(
      L"NSSM/3 health-check", WINHTTP_ACCESS_TYPE_NO_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (! session) return failure(GetLastError(), L"WinHttpOpen failed");
  if (! WinHttpSetTimeouts(session.get(), config.timeout_ms,
                           config.timeout_ms, config.timeout_ms,
                           config.timeout_ms)) {
    return failure(GetLastError(), L"WinHttpSetTimeouts failed");
  }
  unique_winhttp_handle connection(
      WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
  if (! connection) {
    return failure(GetLastError(), L"WinHttpConnect failed");
  }
  unsigned long flags = components.nScheme == INTERNET_SCHEME_HTTPS
                            ? WINHTTP_FLAG_SECURE
                            : 0;
  unique_winhttp_handle request(WinHttpOpenRequest(
      connection.get(), L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
  if (! request) {
    return failure(GetLastError(), L"WinHttpOpenRequest failed");
  }
  bool sent = WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request.get(), nullptr);
  if (! sent) {
    return failure(GetLastError(), L"HTTP request failed");
  }
  unsigned long status = 0;
  unsigned long size = sizeof(status);
  bool queried = WinHttpQueryHeaders(
      request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  unsigned long error = queried ? ERROR_SUCCESS : GetLastError();
  if (! queried) return failure(error, L"reading HTTP status failed");
  if (status < config.minimum_status || status > config.maximum_status) {
    return failure(ERROR_BAD_NET_RESP,
                   L"unexpected HTTP status " + std::to_wstring(status));
  }
  return {true, ERROR_SUCCESS,
          L"HTTP status " + std::to_wstring(status)};
}

health_probe_result_t check_command(const health_check_config_t &config,
                                    const wchar_t *environment,
                                    const wchar_t *working_directory) {
  std::wstring command_line;
  std::wstring error_text;
  if (! build_child_command_line(config.command_path,
                                 config.command_arguments, &command_line,
                                 &error_text)) {
    return failure(ERROR_INVALID_PARAMETER, std::move(error_text));
  }
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  unsigned long error = ERROR_SUCCESS;
  unique_handle job(create_service_job(&error));
  if (! job) return failure(error, L"creating command probe job failed");
  bool created = CreateProcessW(
      config.command_path.c_str(), command_line.data(), nullptr, nullptr, FALSE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
      const_cast<wchar_t *>(environment), working_directory, &startup, &process);
  if (! created) {
    error = GetLastError();
    return failure(error, L"starting command probe failed");
  }
  unique_handle process_thread(process.hThread);
  unique_handle process_handle(process.hProcess);
  if (! assign_process_to_service_job(job.get(), process_handle.get(), &error)) {
    TerminateProcess(process_handle.get(), error);
    return failure(error, L"assigning command probe job failed");
  }
  if (ResumeThread(process_thread.get()) == static_cast<unsigned long>(-1)) {
    error = GetLastError();
    terminate_service_job(job.get(), error, nullptr);
    WaitForSingleObject(process_handle.get(), 5000);
    return failure(error, L"resuming command probe failed");
  }
  process_thread.reset();
  unsigned long wait = WaitForSingleObject(process_handle.get(), config.timeout_ms);
  if (wait != WAIT_OBJECT_0) {
    error = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
    terminate_service_job(job.get(), error, nullptr);
    WaitForSingleObject(process_handle.get(), 5000);
    return failure(error, wait == WAIT_TIMEOUT ? L"command probe timed out"
                                                : L"waiting for command probe failed");
  }
  unsigned long exit_code = 0;
  bool got_exit = GetExitCodeProcess(process_handle.get(), &exit_code) != FALSE;
  error = got_exit ? ERROR_SUCCESS : GetLastError();
  if (! got_exit) return failure(error, L"reading command probe exit code failed");
  if (exit_code) {
    return failure(ERROR_PROCESS_ABORTED,
                   L"command probe exited with code " +
                       std::to_wstring(exit_code));
  }
  return {true, ERROR_SUCCESS, L"command probe succeeded"};
}

health_probe_result_t check_file(const health_check_config_t &config) {
  WIN32_FILE_ATTRIBUTE_DATA attributes = {};
  if (! GetFileAttributesExW(config.file_path.c_str(), GetFileExInfoStandard,
                             &attributes)) {
    return failure(GetLastError(), L"heartbeat file is unavailable");
  }
  if (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    return failure(ERROR_DIRECTORY, L"heartbeat path is a directory");
  }
  FILETIME now_file = {};
  GetSystemTimeAsFileTime(&now_file);
  ULARGE_INTEGER now = {};
  now.LowPart = now_file.dwLowDateTime;
  now.HighPart = now_file.dwHighDateTime;
  ULARGE_INTEGER modified = {};
  modified.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
  modified.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
  unsigned long long age_ms =
      now.QuadPart > modified.QuadPart
          ? (now.QuadPart - modified.QuadPart) / 10000ULL
          : 0;
  if (age_ms > config.maximum_age_ms) {
    return failure(ERROR_TIMEOUT,
                   L"heartbeat file is stale by " +
                       std::to_wstring(age_ms) + L" ms");
  }
  return {true, ERROR_SUCCESS, L"heartbeat file is fresh"};
}

}  // namespace

health_probe_result_t run_health_probe(const health_check_config_t &config,
                                       HANDLE application_process,
                                       const wchar_t *environment,
                                       const wchar_t *working_directory) {
  switch (config.type) {
    case health_check_type_t::process:
      return check_process(application_process);
    case health_check_type_t::tcp:
      return check_tcp(config);
    case health_check_type_t::http:
      return check_http(config);
    case health_check_type_t::command:
      return check_command(config, environment, working_directory);
    case health_check_type_t::file:
      return check_file(config);
    case health_check_type_t::disabled:
    default:
      return {true, ERROR_SUCCESS, L"health checking is disabled"};
  }
}
