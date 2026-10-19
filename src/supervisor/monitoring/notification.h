#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include <windows.h>

#include <string>

struct nssm_service_t;

bool start_notification_listener(nssm_service_t *, std::wstring *);
void stop_notification_listener(nssm_service_t *);
bool wait_for_ready_notification(nssm_service_t *, unsigned long,
                                 std::wstring *);
bool start_notification_watchdog(nssm_service_t *);

#endif
