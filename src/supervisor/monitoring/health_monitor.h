#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

struct service_runtime_t;

bool start_health_monitor(service_runtime_t *service);
void stop_health_monitor(service_runtime_t *service);

#endif
