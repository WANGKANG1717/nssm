#include "event.h"
#include "messages.h"
#include "service.h"
#include "service_control.h"
#include "service_constants.h"

const wchar_t *service_control_text(unsigned long control) {
  switch (control) {
    /* There is no native SERVICE_CONTROL_START constant. */
    case kServiceControlStart: return L"START";
    case SERVICE_CONTROL_STOP: return L"STOP";
    case SERVICE_CONTROL_SHUTDOWN: return L"SHUTDOWN";
    case SERVICE_CONTROL_PAUSE: return L"PAUSE";
    case SERVICE_CONTROL_CONTINUE: return L"CONTINUE";
    case SERVICE_CONTROL_INTERROGATE: return L"INTERROGATE";
    case kServiceControlRotate: return L"ROTATE";
    case kServiceControlReload: return L"RELOAD";
    case SERVICE_CONTROL_POWEREVENT: return L"POWEREVENT";
    default: return nullptr;
  }
}

const wchar_t *service_status_text(unsigned long status) {
  switch (status) {
    case SERVICE_STOPPED: return L"SERVICE_STOPPED";
    case SERVICE_START_PENDING: return L"SERVICE_START_PENDING";
    case SERVICE_STOP_PENDING: return L"SERVICE_STOP_PENDING";
    case SERVICE_RUNNING: return L"SERVICE_RUNNING";
    case SERVICE_CONTINUE_PENDING: return L"SERVICE_CONTINUE_PENDING";
    case SERVICE_PAUSE_PENDING: return L"SERVICE_PAUSE_PENDING";
    case SERVICE_PAUSED: return L"SERVICE_PAUSED";
    default: return L"SERVICE_UNKNOWN";
  }
}

void log_service_control(const wchar_t *service_name, unsigned long control,
                         bool handled) {
  const wchar_t *text = service_control_text(control);
  unsigned long event = NSSM_EVENT_SERVICE_CONTROL_UNKNOWN;
  wchar_t unknown_control[11] = {};

  if (! text) {
    /* "0x" + 8 hexadecimal digits + NUL. */
    _snwprintf_s(unknown_control, _countof(unknown_control), _TRUNCATE,
                 L"0x%08lx", control);
    text = unknown_control;
  }
  else {
    event = handled ? NSSM_EVENT_SERVICE_CONTROL_HANDLED
                    : NSSM_EVENT_SERVICE_CONTROL_NOT_HANDLED;
  }

  log_event(EVENTLOG_INFORMATION_TYPE, event, service_name, text, 0);
}
