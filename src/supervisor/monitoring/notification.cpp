#include "notification.h"

#include "config.h"
#include "service.h"
#include "job_object.h"
#include "win32_handle.h"

#include <sddl.h>

#include <string>
#include <vector>

namespace {

constexpr unsigned long kNotificationWatchdogExitCode = 0xE0010002UL;

std::wstring notification_account(const service_runtime_t *service) {
  const auto &account = service->config->service.account;
  switch (account.type) {
    case service_account_type_t::system: return L"SYSTEM";
    case service_account_type_t::local_service: return L"LOCAL SERVICE";
    case service_account_type_t::network_service: return L"NETWORK SERVICE";
    case service_account_type_t::virtual_account:
      return L"NT SERVICE\\" + service->config->service.name;
    case service_account_type_t::gmsa:
    case service_account_type_t::user: return account.name;
  }
  return L"";
}

bool notification_descriptor(const service_runtime_t *service,
                             unique_local_memory<void> *descriptor) {
  const std::wstring account = notification_account(service);
  unsigned long sid_bytes = 0;
  unsigned long domain_chars = 0;
  SID_NAME_USE use = SidTypeUnknown;
  LookupAccountNameW(nullptr, account.c_str(), nullptr, &sid_bytes, nullptr,
                     &domain_chars, &use);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || ! sid_bytes) return false;
  std::vector<unsigned char> sid(sid_bytes);
  std::vector<wchar_t> domain(domain_chars ? domain_chars : 1);
  if (! LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sid_bytes,
                           domain.data(), &domain_chars, &use)) return false;
  unique_local_memory<wchar_t> sid_text;
  if (! ConvertSidToStringSidW(sid.data(), sid_text.put())) return false;
  const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" +
                            std::wstring(sid_text.get()) + L")";
  return ConvertStringSecurityDescriptorToSecurityDescriptorW(
             sddl.c_str(), SDDL_REVISION_1, descriptor->put(), nullptr) != FALSE;
}

unsigned long WINAPI notification_thread(void *context) {
  auto *service = static_cast<service_runtime_t *>(context);
  while (WaitForSingleObject(service->notification_stop_event, 0) !=
         WAIT_OBJECT_0) {
    unique_local_memory<void> descriptor;
    if (! notification_descriptor(service, &descriptor)) return GetLastError();
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor.get(),
                                      FALSE};
    unique_handle pipe(CreateNamedPipeW(
        service->notification_pipe,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, 0, 4096, 1000, &attributes));
    if (! pipe) return GetLastError();
    unique_handle io_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (! io_event) return GetLastError();
    OVERLAPPED overlapped = {};
    overlapped.hEvent = io_event.get();
    const BOOL connected = ConnectNamedPipe(pipe.get(), &overlapped);
    unsigned long connect_error = connected ? ERROR_SUCCESS : GetLastError();
    if (! connected && connect_error == ERROR_IO_PENDING) {
      HANDLE handles[] = {service->notification_stop_event, io_event.get()};
      const unsigned long wait = WaitForMultipleObjects(2, handles, FALSE,
                                                        INFINITE);
      if (wait == WAIT_OBJECT_0) {
        CancelIoEx(pipe.get(), &overlapped);
        return 0;
      }
      if (wait != WAIT_OBJECT_0 + 1) return GetLastError();
      unsigned long transferred = 0;
      if (! GetOverlappedResult(pipe.get(), &overlapped, &transferred,
                                FALSE)) {
        connect_error = GetLastError();
      }
      else connect_error = ERROR_SUCCESS;
    }
    if (! connected && connect_error != ERROR_SUCCESS &&
        connect_error != ERROR_PIPE_CONNECTED) {
      return connect_error;
    }
    char buffer[4097] = {};
    unsigned long read = 0;
    ResetEvent(io_event.get());
    ZeroMemory(&overlapped, sizeof(overlapped));
    overlapped.hEvent = io_event.get();
    BOOL read_ok = ReadFile(pipe.get(), buffer, 4096, &read, &overlapped);
    unsigned long read_error = read_ok ? ERROR_SUCCESS : GetLastError();
    if (! read_ok && read_error == ERROR_IO_PENDING) {
      HANDLE handles[] = {service->notification_stop_event, io_event.get()};
      const unsigned long wait = WaitForMultipleObjects(2, handles, FALSE,
                                                        INFINITE);
      if (wait == WAIT_OBJECT_0) {
        CancelIoEx(pipe.get(), &overlapped);
        return 0;
      }
      read_ok = wait == WAIT_OBJECT_0 + 1 &&
                GetOverlappedResult(pipe.get(), &overlapped, &read, FALSE);
    }
    if (read_ok && read) {
      std::string message(buffer, read);
      if (message.find("READY=1") != std::string::npos) {
        SetEvent(service->notification_ready_event);
      }
      if (message.find("WATCHDOG=1") != std::string::npos) {
        SetEvent(service->notification_watchdog_event);
      }
    }
    DisconnectNamedPipe(pipe.get());
  }
  return 0;
}

unsigned long WINAPI watchdog_thread(void *context) {
  auto *service = static_cast<service_runtime_t *>(context);
  const unsigned long timeout = service->config->lifecycle.watchdog_ms;
  HANDLE handles[] = {service->notification_stop_event,
                      service->notification_watchdog_event};
  while (true) {
    const unsigned long wait = WaitForMultipleObjects(2, handles, FALSE,
                                                       timeout);
    if (wait == WAIT_OBJECT_0) return 0;
    if (wait == WAIT_OBJECT_0 + 1) continue;
    if (wait == WAIT_TIMEOUT) {
      unsigned long error = ERROR_SUCCESS;
      if (service->job_handle) {
        terminate_service_job(service->job_handle,
                              kNotificationWatchdogExitCode, &error);
      }
      return kNotificationWatchdogExitCode;
    }
    return GetLastError();
  }
}

}  // namespace

bool start_notification_listener(service_runtime_t *service,
                                 std::wstring *endpoint) {
  if (! service || ! endpoint) return false;
  _snwprintf_s(service->notification_pipe,
               _countof(service->notification_pipe), _TRUNCATE,
               L"\\\\.\\pipe\\SvcPilot.Notify.%lu.%ls", GetCurrentProcessId(),
               service->name);
  service->notification_ready_event =
      CreateEventW(nullptr, TRUE, FALSE, nullptr);
  service->notification_watchdog_event =
      CreateEventW(nullptr, FALSE, FALSE, nullptr);
  service->notification_stop_event =
      CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (! service->notification_ready_event ||
      ! service->notification_watchdog_event ||
      ! service->notification_stop_event) {
    stop_notification_listener(service);
    return false;
  }
  service->notification_thread = CreateThread(
      nullptr, 0, notification_thread, service, 0, nullptr);
  if (! service->notification_thread) {
    stop_notification_listener(service);
    return false;
  }
  *endpoint = service->notification_pipe;
  return true;
}

void stop_notification_listener(service_runtime_t *service) {
  if (! service) return;
  if (service->notification_stop_event) SetEvent(service->notification_stop_event);
  if (service->notification_pipe[0]) {
    unique_handle wake(CreateFileW(service->notification_pipe, GENERIC_WRITE, 0,
                                   nullptr, OPEN_EXISTING, 0, nullptr));
    if (wake) {
      const char stop = '\0';
      unsigned long written = 0;
      WriteFile(wake.get(), &stop, sizeof(stop), &written, nullptr);
    }
  }
  if (service->notification_thread &&
      GetThreadId(service->notification_thread) != GetCurrentThreadId()) {
    WaitForSingleObject(service->notification_thread, INFINITE);
  }
  if (service->notification_watchdog_thread &&
      GetThreadId(service->notification_watchdog_thread) !=
          GetCurrentThreadId()) {
    WaitForSingleObject(service->notification_watchdog_thread, INFINITE);
  }
  if (service->notification_thread) CloseHandle(service->notification_thread);
  if (service->notification_watchdog_thread) {
    CloseHandle(service->notification_watchdog_thread);
  }
  if (service->notification_ready_event) CloseHandle(service->notification_ready_event);
  if (service->notification_watchdog_event) CloseHandle(service->notification_watchdog_event);
  if (service->notification_stop_event) CloseHandle(service->notification_stop_event);
  service->notification_thread = nullptr;
  service->notification_watchdog_thread = nullptr;
  service->notification_ready_event = nullptr;
  service->notification_watchdog_event = nullptr;
  service->notification_stop_event = nullptr;
  service->notification_pipe[0] = L'\0';
}

bool start_notification_watchdog(service_runtime_t *service) {
  if (! service || ! service->config ||
      ! service->config->lifecycle.watchdog_ms) return true;
  service->notification_watchdog_thread =
      CreateThread(nullptr, 0, watchdog_thread, service, 0, nullptr);
  return service->notification_watchdog_thread != nullptr;
}

bool wait_for_ready_notification(service_runtime_t *service,
                                 unsigned long timeout_ms,
                                 std::wstring *detail) {
  HANDLE handles[] = {service->notification_ready_event,
                      service->process_handle, service->stop_event};
  const unsigned long wait = WaitForMultipleObjects(3, handles, FALSE,
                                                     timeout_ms);
  if (wait == WAIT_OBJECT_0) {
    if (detail) *detail = L"application sent READY=1";
    return true;
  }
  if (detail) {
    *detail = wait == WAIT_TIMEOUT ? L"READY=1 notification timed out"
              : wait == WAIT_OBJECT_0 + 1 ? L"application exited before READY=1"
              : wait == WAIT_OBJECT_0 + 2 ? L"service stop was requested"
                                          : L"notification wait failed";
  }
  return false;
}
