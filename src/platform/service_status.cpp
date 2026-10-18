#include "service_status.h"

#include "event.h"
#include "messages.h"

bool report_service_status(SERVICE_STATUS_HANDLE status_handle,
                           SERVICE_STATUS *status,
                           const wchar_t *service_name,
                           const wchar_t *operation) {
  if (status_handle && status && SetServiceStatus(status_handle, status)) {
    return true;
  }

  const unsigned long error = status_handle && status
                                  ? GetLastError()
                                  : ERROR_INVALID_PARAMETER;
  log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_SETSERVICESTATUS_FAILED,
            service_name ? service_name : L"", operation ? operation : L"",
            error_string(error), 0);
  SetLastError(error);
  return false;
}
