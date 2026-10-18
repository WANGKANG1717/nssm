#ifndef CONFIG_SECURITY_H
#define CONFIG_SECURITY_H

#include <string>

struct config_validation_t;
struct service_config_t;

bool audit_secure_path(const std::wstring &, const wchar_t *, bool,
                       config_validation_t *);
bool audit_service_paths(const std::wstring &, const service_config_t &,
                         config_validation_t *);

#endif
