#include "config_parse_utils.h"

#include <windows.h>

#include <cstring>

namespace {

std::wstring field_path(const wchar_t *object, const char *field) {
  bool valid = false;
  std::wstring converted = utf8_to_wide(field, &valid);
  return std::wstring(object) + L"." +
         (valid ? converted : L"<invalid UTF-8>");
}

}  // namespace

std::wstring utf8_to_wide(const char *value, size_t length, bool *valid) {
  if (valid) *valid = false;
  if (! value) return L"";
  if (! length) {
    if (valid) *valid = true;
    return L"";
  }

  const int needed = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value, static_cast<int>(length), nullptr,
      0);
  if (needed <= 0) return L"";
  std::wstring result(static_cast<size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value,
                          static_cast<int>(length), result.data(), needed) !=
      needed) {
    return L"";
  }
  if (valid) *valid = true;
  return result;
}

std::wstring utf8_to_wide(const char *value, bool *valid) {
  return utf8_to_wide(value, value ? std::strlen(value) : 0, valid);
}

void reject_unknown_fields(yyjson_val *object, const wchar_t *object_name,
                           const std::set<std::string> &allowed,
                           config_validation_t *validation) {
  std::set<std::string> seen;
  yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
  yyjson_val *key = nullptr;
  while ((key = yyjson_obj_iter_next(&iterator))) {
    const char *name = yyjson_get_str(key);
    if (name && ! seen.insert(name).second) {
      validation->add(L"Duplicate configuration field: " +
                      field_path(object_name, name));
    }
    else if (name && ! allowed.contains(name)) {
      validation->add(L"Unknown configuration field: " +
                      field_path(object_name, name));
    }
  }
}

bool read_required_string(yyjson_val *object, const char *name,
                          const wchar_t *path, std::wstring *destination,
                          config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! yyjson_is_str(value)) {
    validation->add(std::wstring(path) + L" must be a string");
    return false;
  }
  bool valid = false;
  *destination = utf8_to_wide(yyjson_get_str(value), yyjson_get_len(value),
                              &valid);
  if (! valid) {
    validation->add(std::wstring(path) + L" is not valid UTF-8");
    return false;
  }
  if (destination->find(L'\0') != std::wstring::npos) {
    validation->add(std::wstring(path) + L" must not contain U+0000");
    return false;
  }
  return true;
}

bool read_optional_string(yyjson_val *object, const char *name,
                          const wchar_t *path, std::wstring *destination,
                          config_validation_t *validation) {
  if (! yyjson_obj_get(object, name)) return true;
  return read_required_string(object, name, path, destination, validation);
}

void read_optional_number(yyjson_val *object, const char *name,
                          const wchar_t *path, unsigned long minimum,
                          unsigned long maximum, unsigned long *destination,
                          config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! value) return;
  if (! yyjson_is_uint(value) || yyjson_get_uint(value) < minimum ||
      yyjson_get_uint(value) > maximum) {
    validation->add(std::wstring(path) + L" is outside the allowed range");
    return;
  }
  *destination = static_cast<unsigned long>(yyjson_get_uint(value));
}

bool read_required_number(yyjson_val *object, const char *name,
                          const wchar_t *path, unsigned long minimum,
                          unsigned long maximum, unsigned long *destination,
                          config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! yyjson_is_uint(value) || yyjson_get_uint(value) < minimum ||
      yyjson_get_uint(value) > maximum) {
    validation->add(std::wstring(path) +
                    L" is required and outside the allowed range");
    return false;
  }
  *destination = static_cast<unsigned long>(yyjson_get_uint(value));
  return true;
}

void read_optional_number64(yyjson_val *object, const char *name,
                            const wchar_t *path,
                            unsigned long long maximum,
                            unsigned long long *destination,
                            config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! value) return;
  if (! yyjson_is_uint(value) || yyjson_get_uint(value) > maximum) {
    validation->add(std::wstring(path) + L" is outside the allowed range");
    return;
  }
  *destination = yyjson_get_uint(value);
}

void read_optional_bool(yyjson_val *object, const char *name,
                        const wchar_t *path, bool *destination,
                        config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! value) return;
  if (! yyjson_is_bool(value)) {
    validation->add(std::wstring(path) + L" must be a boolean");
    return;
  }
  *destination = yyjson_get_bool(value);
}
