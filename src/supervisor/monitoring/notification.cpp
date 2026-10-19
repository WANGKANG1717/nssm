#include "notification.h"

#include "config.h"
#include "service.h"
#include "job_object.h"
#include "win32_handle.h"

#include <sddl.h>

#include <string>

namespace {

constexpr unsigned long kNotificationWatchdogExitCode = 0xE0010002UL;

unsigned long WINAPI notification_thread(void *context) {
  auto *service = static_cast<nssm_service_t *>(context);
  while (WaitForSingleObject(service->notification_stop_event, 0) !=
         WAIT_OBJECT_0) {
    unique_local_memory<void> descriptor;
    if (! ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)", SDDL_REVISION_1,
            descriptor.put(), nullptr)) return GetLastError();
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor.get(),
                                      FALSE};
    unique_handle pipe(CreateNamedPipeW(
        service->notification_pipe,
        PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, 0, 4096, 1000, &attributes));
    if (! pipe) return GetLastError();
    const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr);
    const unsigned long connect_error = connected ? ERROR_SUCCESS : GetLastError();
    if (! connected && connect_error != ERROR_PIPE_CONNECTED) {
      if (connect_error == ERROR_OPERATION_ABORTED) return 0;
      return connect_error;
    }
    char buffer[4097] = {};
    unsigned long read = 0;
    if (ReadFile(pipe.get(), buffer, 4096, &read, nullptr) && read) {
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
  auto *service = static_cast<nssm_service_t *>(context);
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

bool start_notification_listener(nssm_service_t *service,
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

void stop_notification_listener(nssm_service_t *service) {
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

bool start_notification_watchdog(nssm_service_t *service) {
  if (! service || ! service->config ||
      ! service->config->lifecycle.watchdog_ms) return true;
  service->notification_watchdog_thread =
      CreateThread(nullptr, 0, watchdog_thread, service, 0, nullptr);
  return service->notification_watchdog_thread != nullptr;
}

bool wait_for_ready_notification(nssm_service_t *service,
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
