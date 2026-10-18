#ifndef SERVICE_STATUS_H
#define SERVICE_STATUS_H

#include <windows.h>

bool report_service_status(SERVICE_STATUS_HANDLE, SERVICE_STATUS *,
                           const wchar_t *, const wchar_t *);

#endif
