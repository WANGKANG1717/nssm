#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include <windows.h>

#include <string>

struct service_runtime_t;

bool start_notification_listener(service_runtime_t *, std::wstring *);
void stop_notification_listener(service_runtime_t *);
bool wait_for_ready_notification(service_runtime_t *, unsigned long,
                                 std::wstring *);
bool start_notification_watchdog(service_runtime_t *);

#endif
