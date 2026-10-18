#ifndef NSSM_SECURITY_H
#define NSSM_SECURITY_H

#include <string>

struct config_validation_t;
struct service_config_t;

bool audit_secure_path(const std::wstring &path, const wchar_t *field,
                       bool must_exist, config_validation_t *validation);
bool audit_service_paths(const std::wstring &config_path,
                         const service_config_t &config,
                         config_validation_t *validation);

#endif
