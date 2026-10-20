#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

struct service_runtime_t;

bool start_resource_monitor(service_runtime_t *service);
void stop_resource_monitor(service_runtime_t *service);

#endif
