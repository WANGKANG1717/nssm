#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

struct nssm_service_t;

bool start_health_monitor(nssm_service_t *service);
void stop_health_monitor(nssm_service_t *service);

#endif
