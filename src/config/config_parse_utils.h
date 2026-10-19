#ifndef CONFIG_PARSE_UTILS_H
#define CONFIG_PARSE_UTILS_H

#include "config.h"

#include <set>
#include <string>
#include <string_view>

#include <toml++/toml.hpp>

std::wstring utf8_to_wide(const char *, size_t, bool *);
std::wstring utf8_to_wide(const char *, bool * = nullptr);
std::wstring utf8_to_wide(std::string_view, bool * = nullptr);
void reject_unknown_fields(const toml::table *, const wchar_t *,
                           const std::set<std::string> &,
                           config_validation_t *);
bool read_required_string(const toml::table *, const char *, const wchar_t *,
                          std::wstring *, config_validation_t *);
bool read_optional_string(const toml::table *, const char *, const wchar_t *,
                          std::wstring *, config_validation_t *);
void read_optional_number(const toml::table *, const char *, const wchar_t *,
                          unsigned long, unsigned long, unsigned long *,
                          config_validation_t *);
bool read_required_number(const toml::table *, const char *, const wchar_t *,
                          unsigned long, unsigned long, unsigned long *,
                          config_validation_t *);
void read_optional_number64(const toml::table *, const char *,
                            const wchar_t *, unsigned long long,
                            unsigned long long *, config_validation_t *);
void read_optional_bool(const toml::table *, const char *, const wchar_t *,
                        bool *, config_validation_t *);

#endif
