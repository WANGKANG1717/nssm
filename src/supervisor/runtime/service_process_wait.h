#ifndef SVP_SERVICE_PROCESS_WAIT_H
#define SVP_SERVICE_PROCESS_WAIT_H

struct service_runtime_t;

bool register_service_process_wait(service_runtime_t *);
void unregister_service_process_wait(service_runtime_t *, bool);

#endif
