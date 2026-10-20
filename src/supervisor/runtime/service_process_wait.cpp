#include "service_process_wait.h"

#include "event.h"
#include "messages.h"
#include "service.h"

#include <windows.h>

void unregister_service_process_wait(service_runtime_t *service,
                                     bool wait_for_callbacks) {
  AcquireSRWLockExclusive(&service->wait_lock);
  HANDLE wait_handle = static_cast<HANDLE>(InterlockedExchangePointer(
      reinterpret_cast<void *volatile *>(&service->wait_handle), nullptr));
  ReleaseSRWLockExclusive(&service->wait_lock);
  if (! wait_handle) return;

  HANDLE completion_event = wait_for_callbacks ? INVALID_HANDLE_VALUE : nullptr;
  if (UnregisterWaitEx(wait_handle, completion_event)) return;
  const unsigned long error = GetLastError();
  if (! wait_for_callbacks && error == ERROR_IO_PENDING) return;
  log_event(EVENTLOG_ERROR_TYPE, SVP_EVENT_UNREGISTERWAIT_FAILED,
            service->name, error_string(error), 0);
}

bool register_service_process_wait(service_runtime_t *service) {
  AcquireSRWLockExclusive(&service->wait_lock);
  if (! service->process_handle ||
      WaitForSingleObject(service->stop_event, 0) == WAIT_OBJECT_0) {
    ReleaseSRWLockExclusive(&service->wait_lock);
    SetLastError(ERROR_OPERATION_ABORTED);
    return false;
  }

  const bool registered = RegisterWaitForSingleObject(
      &service->wait_handle, service->process_handle, end_service, service,
      INFINITE, WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION);
  const unsigned long error = registered ? ERROR_SUCCESS : GetLastError();
  ReleaseSRWLockExclusive(&service->wait_lock);
  SetLastError(error);
  return registered;
}
