#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

struct nssm_service_t;

bool start_resource_monitor(nssm_service_t *service);
void stop_resource_monitor(nssm_service_t *service);

#endif
