#ifndef SERVICE_CONTROL_H
#define SERVICE_CONTROL_H

#include <windows.h>

const wchar_t *service_control_text(unsigned long);
const wchar_t *service_status_text(unsigned long);
void log_service_control(const wchar_t *, unsigned long, bool);

#endif
