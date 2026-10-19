#ifndef CONFIG_REGISTRATION_H
#define CONFIG_REGISTRATION_H

#include <string>

struct config_validation_t;

bool full_config_path(const wchar_t *, std::wstring *, config_validation_t *);
bool write_config_path(const std::wstring &, const std::wstring &,
                       unsigned long *);
bool read_config_path(const std::wstring &, std::wstring *, unsigned long *);
bool registered_service_config_path(const wchar_t *service_name,
                                    std::wstring *path,
                                    unsigned long *error);

struct persistent_failure_t {
  bool failed = false;
  unsigned long exit_code = 0;
  unsigned long long timestamp_100ns = 0;
};

bool write_persistent_failure(const std::wstring &, unsigned long,
                              unsigned long *);
bool read_persistent_failure(const std::wstring &, persistent_failure_t *,
                             unsigned long *);
bool clear_persistent_failure(const std::wstring &, unsigned long *);

#endif
