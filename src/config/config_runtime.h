#ifndef NSSM_CONFIG_RUNTIME_H
#define NSSM_CONFIG_RUNTIME_H

#include "config.h"

struct nssm_service_t;

bool load_registered_service_config(nssm_service_t *service,
                                    config_validation_t *validation);

#endif
