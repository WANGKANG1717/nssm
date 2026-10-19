#include "config_parse_utils.h"

#include <windows.h>

#include <cstring>
#include <optional>

namespace {

std::wstring field_path(const wchar_t *object, std::string_view field) {
  bool valid = false;
  std::wstring converted = utf8_to_wide(field, &valid);
  return std::wstring(object) + L"." +
         (valid ? converted : L"<invalid UTF-8>");
}

const toml::node *field(const toml::table *object, const char *name) {
  return object ? object->get(name) : nullptr;
}

bool integer_value(const toml::node *node, unsigned long long maximum,
                   unsigned long long *result) {
  if (! node) return false;
  const auto value = node->value<int64_t>();
  if (! value || *value < 0 ||
      static_cast<unsigned long long>(*value) > maximum) {
    return false;
  }
  *result = static_cast<unsigned long long>(*value);
  return true;
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

std::wstring utf8_to_wide(std::string_view value, bool *valid) {
  return utf8_to_wide(value.data(), value.size(), valid);
}

void reject_unknown_fields(const toml::table *object,
                           const wchar_t *object_name,
                           const std::set<std::string> &allowed,
                           config_validation_t *validation) {
  if (! object) return;
  for (const auto &[key, value] : *object) {
    (void)value;
    const std::string name(key.str());
    if (! allowed.contains(name)) {
      validation->add(L"Unknown configuration field: " +
                      field_path(object_name, name));
    }
  }
}

bool read_required_string(const toml::table *object, const char *name,
                          const wchar_t *path, std::wstring *destination,
                          config_validation_t *validation) {
  const toml::node *node = field(object, name);
  const auto value = node ? node->value<std::string>() : std::nullopt;
  if (! value) {
    validation->add(std::wstring(path) + L" must be a string");
    return false;
  }
  bool valid = false;
  *destination = utf8_to_wide(*value, &valid);
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

bool read_optional_string(const toml::table *object, const char *name,
                          const wchar_t *path, std::wstring *destination,
                          config_validation_t *validation) {
  if (! field(object, name)) return true;
  return read_required_string(object, name, path, destination, validation);
}

void read_optional_number(const toml::table *object, const char *name,
                          const wchar_t *path, unsigned long minimum,
                          unsigned long maximum, unsigned long *destination,
                          config_validation_t *validation) {
  const toml::node *node = field(object, name);
  if (! node) return;
  unsigned long long value = 0;
  if (! integer_value(node, maximum, &value) || value < minimum) {
    validation->add(std::wstring(path) + L" is outside the allowed range");
    return;
  }
  *destination = static_cast<unsigned long>(value);
}

bool read_required_number(const toml::table *object, const char *name,
                          const wchar_t *path, unsigned long minimum,
                          unsigned long maximum, unsigned long *destination,
                          config_validation_t *validation) {
  unsigned long long value = 0;
  if (! integer_value(field(object, name), maximum, &value) ||
      value < minimum) {
    validation->add(std::wstring(path) +
                    L" is required and outside the allowed range");
    return false;
  }
  *destination = static_cast<unsigned long>(value);
  return true;
}

void read_optional_number64(const toml::table *object, const char *name,
                            const wchar_t *path,
                            unsigned long long maximum,
                            unsigned long long *destination,
                            config_validation_t *validation) {
  const toml::node *node = field(object, name);
  if (! node) return;
  unsigned long long value = 0;
  if (! integer_value(node, maximum, &value)) {
    validation->add(std::wstring(path) + L" is outside the allowed range");
    return;
  }
  *destination = value;
}

void read_optional_bool(const toml::table *object, const char *name,
                        const wchar_t *path, bool *destination,
                        config_validation_t *validation) {
  const toml::node *node = field(object, name);
  if (! node) return;
  const auto value = node->value<bool>();
  if (! value) {
    validation->add(std::wstring(path) + L" must be a boolean");
    return;
  }
  *destination = *value;
}
