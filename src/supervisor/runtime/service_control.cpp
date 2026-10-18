#include "nssm.h"
#include "event.h"
#include "messages.h"
#include "service.h"

const TCHAR *service_control_text(unsigned long control) {
  switch (control) {
    /* There is no native SERVICE_CONTROL_START constant. */
    case NSSM_SERVICE_CONTROL_START: return _T("START");
    case SERVICE_CONTROL_STOP: return _T("STOP");
    case SERVICE_CONTROL_SHUTDOWN: return _T("SHUTDOWN");
    case SERVICE_CONTROL_PAUSE: return _T("PAUSE");
    case SERVICE_CONTROL_CONTINUE: return _T("CONTINUE");
    case SERVICE_CONTROL_INTERROGATE: return _T("INTERROGATE");
    case NSSM_SERVICE_CONTROL_ROTATE: return _T("ROTATE");
    case SERVICE_CONTROL_POWEREVENT: return _T("POWEREVENT");
    default: return nullptr;
  }
}

const TCHAR *service_status_text(unsigned long status) {
  switch (status) {
    case SERVICE_STOPPED: return _T("SERVICE_STOPPED");
    case SERVICE_START_PENDING: return _T("SERVICE_START_PENDING");
    case SERVICE_STOP_PENDING: return _T("SERVICE_STOP_PENDING");
    case SERVICE_RUNNING: return _T("SERVICE_RUNNING");
    case SERVICE_CONTINUE_PENDING: return _T("SERVICE_CONTINUE_PENDING");
    case SERVICE_PAUSE_PENDING: return _T("SERVICE_PAUSE_PENDING");
    case SERVICE_PAUSED: return _T("SERVICE_PAUSED");
    default: return _T("SERVICE_UNKNOWN");
  }
}

void log_service_control(TCHAR *service_name, unsigned long control,
                         bool handled) {
  const TCHAR *text = service_control_text(control);
  unsigned long event = NSSM_EVENT_SERVICE_CONTROL_UNKNOWN;
  TCHAR unknown_control[11] = {};

  if (! text) {
    /* "0x" + 8 hexadecimal digits + NUL. */
    _sntprintf_s(unknown_control, _countof(unknown_control), _TRUNCATE,
                 _T("0x%08lx"), control);
    text = unknown_control;
  }
  else {
    event = handled ? NSSM_EVENT_SERVICE_CONTROL_HANDLED
                    : NSSM_EVENT_SERVICE_CONTROL_NOT_HANDLED;
  }

  log_event(EVENTLOG_INFORMATION_TYPE, event, service_name, text, 0);
}
