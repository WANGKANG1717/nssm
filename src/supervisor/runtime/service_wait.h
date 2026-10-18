#ifndef SERVICE_WAIT_H
#define SERVICE_WAIT_H

#include <windows.h>
#include <tchar.h>

int await_single_handle(SERVICE_STATUS_HANDLE, SERVICE_STATUS *, HANDLE,
                        const TCHAR *, const TCHAR *, unsigned long,
                        SRWLOCK * = nullptr);

#endif
