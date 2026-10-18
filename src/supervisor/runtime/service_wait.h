#ifndef SERVICE_WAIT_H
#define SERVICE_WAIT_H

#include <windows.h>

int await_single_handle(SERVICE_STATUS_HANDLE, SERVICE_STATUS *, HANDLE,
                        const wchar_t *, const wchar_t *, unsigned long,
                        SRWLOCK * = nullptr);

#endif
