#ifndef CONFIG_REGISTRATION_H
#define CONFIG_REGISTRATION_H

#include <string>

struct config_validation_t;

bool full_config_path(const wchar_t *, std::wstring *, config_validation_t *);
bool write_config_path(const std::wstring &, const std::wstring &,
                       unsigned long *);
bool read_config_path(const std::wstring &, std::wstring *, unsigned long *);

#endif
