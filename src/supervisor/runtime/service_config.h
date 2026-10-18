#ifndef NSSM_SUPERVISOR_SERVICE_CONFIG_H
#define NSSM_SUPERVISOR_SERVICE_CONFIG_H

struct config_validation_t;
struct nssm_service_t;

bool load_registered_service_config(nssm_service_t *service,
                                    config_validation_t *validation);

#endif
