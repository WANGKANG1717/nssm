#ifndef NSSM_STRING_UTILS_H
#define NSSM_STRING_UTILS_H

#include <string>

bool strings_equal_ignore_case(const wchar_t *left,
                               const wchar_t *right) noexcept;
bool string_starts_with_ignore_case(const std::wstring &value,
                                    const wchar_t *prefix) noexcept;

struct ordinal_ignore_case_less {
  bool operator()(const std::wstring &left,
                  const std::wstring &right) const noexcept;
};

#endif
