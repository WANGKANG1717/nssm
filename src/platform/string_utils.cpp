#include "string_utils.h"

#include <windows.h>

bool strings_equal_ignore_case(const wchar_t *left,
                               const wchar_t *right) noexcept {
  if (! left || ! right) return left == right;
  return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}
