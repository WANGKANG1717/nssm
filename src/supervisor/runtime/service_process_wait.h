#ifndef NSSM_SERVICE_PROCESS_WAIT_H
#define NSSM_SERVICE_PROCESS_WAIT_H

struct nssm_service_t;

bool register_service_process_wait(nssm_service_t *);
void unregister_service_process_wait(nssm_service_t *, bool);

#endif
