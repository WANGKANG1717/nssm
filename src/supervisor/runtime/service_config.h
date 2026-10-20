#ifndef SVP_SUPERVISOR_SERVICE_CONFIG_H
#define SVP_SUPERVISOR_SERVICE_CONFIG_H

struct config_validation_t;
struct service_runtime_t;

bool load_registered_service_config(service_runtime_t *service,
                                    config_validation_t *validation);

#endif
