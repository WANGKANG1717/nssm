#include "string_utils.h"

#include <windows.h>

#include <cwchar>

bool strings_equal_ignore_case(const wchar_t *left,
                               const wchar_t *right) noexcept {
  if (! left || ! right) return left == right;
  return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

bool string_starts_with_ignore_case(const std::wstring &value,
                                    const wchar_t *prefix) noexcept {
  if (! prefix) return false;
  const size_t prefix_length = wcslen(prefix);
  if (prefix_length > value.size()) return false;
  return CompareStringOrdinal(value.c_str(), static_cast<int>(prefix_length),
                              prefix, static_cast<int>(prefix_length), TRUE) ==
         CSTR_EQUAL;
}

bool ordinal_ignore_case_less::operator()(
    const std::wstring &left, const std::wstring &right) const noexcept {
  return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
         CSTR_LESS_THAN;
}
