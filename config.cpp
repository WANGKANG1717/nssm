#include "config.h"

#include <windows.h>
#include <shlwapi.h>

#include <cwctype>
#include <set>
#include <string>
#include <vector>

#include <yyjson.h>

namespace {

constexpr unsigned long long kMaximumConfigBytes = 4ULL * 1024ULL * 1024ULL;

struct case_insensitive_less {
  bool operator()(const std::wstring &left, const std::wstring &right) const {
    size_t common = left.size() < right.size() ? left.size() : right.size();
    for (size_t i = 0; i < common; ++i) {
      wchar_t a = static_cast<wchar_t>(std::towupper(left[i]));
      wchar_t b = static_cast<wchar_t>(std::towupper(right[i]));
      if (a != b) return a < b;
    }
    return left.size() < right.size();
  }
};

std::wstring utf8_to_wide(const char *value, size_t length, bool *valid) {
  if (valid) *valid = false;
  if (! value) return L"";
  if (! length) {
    if (valid) *valid = true;
    return L"";
  }

  int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value,
                                   static_cast<int>(length), nullptr, 0);
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

std::wstring utf8_to_wide(const char *value, bool *valid = nullptr) {
  return utf8_to_wide(value, value ? strlen(value) : 0, valid);
}

std::wstring field_path(const wchar_t *object, const char *field) {
  bool valid = false;
  std::wstring converted = utf8_to_wide(field, &valid);
  return std::wstring(object) + L"." + (valid ? converted : L"<invalid UTF-8>");
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
  *destination = utf8_to_wide(yyjson_get_str(value), yyjson_get_len(value), &valid);
  if (! valid) {
    validation->add(std::wstring(path) + L" is not valid UTF-8");
    return false;
  }
  return true;
}

bool read_optional_string(yyjson_val *object, const char *name,
                          const wchar_t *path, std::wstring *destination,
                          config_validation_t *validation) {
  yyjson_val *value = yyjson_obj_get(object, name);
  if (! value) return true;
  return read_required_string(object, name, path, destination, validation);
}

bool valid_service_name(const std::wstring &name) {
  if (name.empty() || name.size() > 256) return false;
  for (wchar_t character : name) {
    if (character == L'/' || character == L'\\' || character < 32) return false;
  }
  return true;
}

void parse_account(yyjson_val *service, service_account_config_t *account,
                   config_validation_t *validation) {
  yyjson_val *object = yyjson_obj_get(service, "account");
  if (! object) return;
  if (! yyjson_is_obj(object)) {
    validation->add(L"service.account must be an object");
    return;
  }
  reject_unknown_fields(object, L"service.account", {"type", "name"},
                        validation);

  std::wstring type;
  if (! read_required_string(object, "type", L"service.account.type", &type,
                             validation)) {
    return;
  }
  if (type == L"system") account->type = service_account_type_t::system;
  else if (type == L"localService") account->type = service_account_type_t::local_service;
  else if (type == L"networkService") account->type = service_account_type_t::network_service;
  else if (type == L"virtual") account->type = service_account_type_t::virtual_account;
  else if (type == L"gmsa") account->type = service_account_type_t::gmsa;
  else if (type == L"user") account->type = service_account_type_t::user;
  else {
    validation->add(L"service.account.type has an unsupported value");
    return;
  }

  read_optional_string(object, "name", L"service.account.name", &account->name,
                       validation);
  bool needs_name = account->type == service_account_type_t::virtual_account ||
                    account->type == service_account_type_t::gmsa ||
                    account->type == service_account_type_t::user;
  if (needs_name && account->name.empty()) {
    validation->add(L"service.account.name is required for this account type");
  }
  if (! needs_name && ! account->name.empty()) {
    validation->add(L"service.account.name is not allowed for this account type");
  }
}

void parse_dependencies(yyjson_val *service,
                        std::vector<std::wstring> *dependencies,
                        config_validation_t *validation) {
  yyjson_val *array = yyjson_obj_get(service, "dependencies");
  if (! array) return;
  if (! yyjson_is_arr(array)) {
    validation->add(L"service.dependencies must be an array of strings");
    return;
  }

  size_t index = 0;
  yyjson_val *value = nullptr;
  yyjson_arr_iter iterator = yyjson_arr_iter_with(array);
  while ((value = yyjson_arr_iter_next(&iterator))) {
    if (! yyjson_is_str(value)) {
      validation->add(L"service.dependencies[" + std::to_wstring(index) +
                      L"] must be a string");
    }
    else {
      bool valid = false;
      std::wstring dependency = utf8_to_wide(
          yyjson_get_str(value), yyjson_get_len(value), &valid);
      if (! valid || dependency.empty()) {
        validation->add(L"service.dependencies[" + std::to_wstring(index) +
                        L"] must be non-empty valid UTF-8");
      }
      else dependencies->push_back(std::move(dependency));
    }
    ++index;
  }
}

void parse_service(yyjson_val *root, service_metadata_config_t *service,
                   config_validation_t *validation) {
  yyjson_val *object = yyjson_obj_get(root, "service");
  if (! yyjson_is_obj(object)) {
    validation->add(L"service must be an object");
    return;
  }
  reject_unknown_fields(object, L"service",
                        {"name", "displayName", "description", "startup",
                         "account", "dependencies"},
                        validation);

  read_required_string(object, "name", L"service.name", &service->name,
                       validation);
  if (! valid_service_name(service->name)) {
    validation->add(L"service.name is empty, too long, or contains an invalid character");
  }
  read_optional_string(object, "displayName", L"service.displayName",
                       &service->display_name, validation);
  read_optional_string(object, "description", L"service.description",
                       &service->description, validation);
  if (service->display_name.empty()) service->display_name = service->name;

  std::wstring startup;
  if (read_optional_string(object, "startup", L"service.startup", &startup,
                           validation) && ! startup.empty()) {
    if (startup == L"automatic") service->startup = service_startup_t::automatic;
    else if (startup == L"delayed") service->startup = service_startup_t::delayed;
    else if (startup == L"manual") service->startup = service_startup_t::manual;
    else if (startup == L"disabled") service->startup = service_startup_t::disabled;
    else validation->add(L"service.startup has an unsupported value");
  }

  parse_account(object, &service->account, validation);
  parse_dependencies(object, &service->dependencies, validation);
}

void parse_arguments(yyjson_val *application,
                     std::vector<std::wstring> *arguments,
                     config_validation_t *validation) {
  yyjson_val *array = yyjson_obj_get(application, "arguments");
  if (! array) return;
  if (! yyjson_is_arr(array)) {
    validation->add(L"application.arguments must be an array of strings");
    return;
  }

  size_t index = 0;
  yyjson_val *value = nullptr;
  yyjson_arr_iter iterator = yyjson_arr_iter_with(array);
  while ((value = yyjson_arr_iter_next(&iterator))) {
    if (! yyjson_is_str(value)) {
      validation->add(L"application.arguments[" + std::to_wstring(index) +
                      L"] must be a string");
    }
    else {
      bool valid = false;
      std::wstring argument = utf8_to_wide(yyjson_get_str(value),
                                           yyjson_get_len(value), &valid);
      if (! valid) {
        validation->add(L"application.arguments[" + std::to_wstring(index) +
                        L"] is not valid UTF-8");
      }
      else arguments->push_back(std::move(argument));
    }
    ++index;
  }
}

void parse_environment(yyjson_val *application,
                       application_config_t *config,
                       config_validation_t *validation) {
  yyjson_val *inherit = yyjson_obj_get(application, "inheritEnvironment");
  if (inherit) {
    if (! yyjson_is_bool(inherit)) {
      validation->add(L"application.inheritEnvironment must be a boolean");
    }
    else config->inherit_environment = yyjson_get_bool(inherit);
  }

  yyjson_val *environment = yyjson_obj_get(application, "environment");
  if (! environment) return;
  if (! yyjson_is_obj(environment)) {
    validation->add(L"application.environment must be an object of string values");
    return;
  }

  std::set<std::wstring, case_insensitive_less> names;
  yyjson_obj_iter iterator = yyjson_obj_iter_with(environment);
  yyjson_val *key = nullptr;
  while ((key = yyjson_obj_iter_next(&iterator))) {
    yyjson_val *value = yyjson_obj_iter_get_val(key);
    bool name_valid = false;
    std::wstring name = utf8_to_wide(yyjson_get_str(key), yyjson_get_len(key),
                                     &name_valid);
    if (! name_valid || name.empty() || name.find(L'=') != std::wstring::npos) {
      validation->add(L"application.environment contains an invalid variable name");
      continue;
    }
    if (! names.insert(name).second) {
      validation->add(L"application.environment contains duplicate variable names ignoring case: " + name);
      continue;
    }
    if (! yyjson_is_str(value)) {
      validation->add(L"application.environment." + name + L" must be a string");
      continue;
    }
    bool value_valid = false;
    std::wstring converted = utf8_to_wide(yyjson_get_str(value),
                                          yyjson_get_len(value), &value_valid);
    if (! value_valid) {
      validation->add(L"application.environment." + name + L" is not valid UTF-8");
      continue;
    }
    config->environment.emplace_back(std::move(name), std::move(converted));
  }
}

void parse_application(yyjson_val *root, application_config_t *application,
                       config_validation_t *validation) {
  yyjson_val *object = yyjson_obj_get(root, "application");
  if (! yyjson_is_obj(object)) {
    validation->add(L"application must be an object");
    return;
  }
  reject_unknown_fields(object, L"application",
                        {"path", "arguments", "workingDirectory",
                         "inheritEnvironment", "environment"},
                        validation);

  read_required_string(object, "path", L"application.path", &application->path,
                       validation);
  if (application->path.empty() || PathIsRelativeW(application->path.c_str())) {
    validation->add(L"application.path must be an absolute path");
  }
  read_optional_string(object, "workingDirectory",
                       L"application.workingDirectory",
                       &application->working_directory, validation);
  if (! application->working_directory.empty() &&
      PathIsRelativeW(application->working_directory.c_str())) {
    validation->add(L"application.workingDirectory must be an absolute path");
  }
  if (application->working_directory.empty() && ! application->path.empty()) {
    size_t separator = application->path.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
      application->working_directory = application->path.substr(0, separator);
    }
  }

  parse_arguments(object, &application->arguments, validation);
  parse_environment(object, application, validation);
}

std::wstring read_error_message(unsigned long error) {
  return L"Unable to read configuration file (Windows error " +
         std::to_wstring(error) + L")";
}

}  // namespace

bool parse_service_config(const char *json, size_t length,
                          service_config_t *config,
                          config_validation_t *validation) {
  if (! config || ! validation) return false;
  *config = {};
  validation->errors.clear();
  if (! json || ! length) {
    validation->add(L"Configuration file is empty");
    return false;
  }

  yyjson_read_err parse_error = {};
  yyjson_doc *document = yyjson_read_opts(const_cast<char *>(json), length, 0,
                                          nullptr, &parse_error);
  if (! document) {
    bool valid = false;
    std::wstring message = utf8_to_wide(parse_error.msg, &valid);
    validation->add(L"Invalid JSON at byte " + std::to_wstring(parse_error.pos) +
                    L": " + (valid ? message : L"parse error"));
    return false;
  }

  yyjson_val *root = yyjson_doc_get_root(document);
  if (! yyjson_is_obj(root)) {
    validation->add(L"Configuration root must be an object");
    yyjson_doc_free(document);
    return false;
  }
  reject_unknown_fields(root, L"root", {"schema", "service", "application"},
                        validation);

  yyjson_val *schema = yyjson_obj_get(root, "schema");
  if (! yyjson_is_uint(schema) || yyjson_get_uint(schema) != 1) {
    validation->add(L"schema must be the integer 1");
  }
  else config->schema = 1;

  parse_service(root, &config->service, validation);
  parse_application(root, &config->application, validation);
  yyjson_doc_free(document);
  return validation->ok();
}

bool load_service_config(const wchar_t *path, service_config_t *config,
                         config_validation_t *validation) {
  if (! config || ! validation) return false;
  validation->errors.clear();
  if (! path || ! *path) {
    validation->add(L"Configuration path must not be empty");
    return false;
  }

  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    validation->add(read_error_message(GetLastError()));
    return false;
  }

  LARGE_INTEGER size = {};
  if (! GetFileSizeEx(file, &size)) {
    unsigned long error = GetLastError();
    CloseHandle(file);
    validation->add(read_error_message(error));
    return false;
  }
  if (size.QuadPart <= 0 || static_cast<unsigned long long>(size.QuadPart) >
                                kMaximumConfigBytes) {
    CloseHandle(file);
    validation->add(L"Configuration file must be between 1 byte and 4 MiB");
    return false;
  }

  std::vector<char> content(static_cast<size_t>(size.QuadPart));
  size_t offset = 0;
  while (offset < content.size()) {
    unsigned long chunk = static_cast<unsigned long>(
        (content.size() - offset) > 1024 * 1024 ? 1024 * 1024
                                                : content.size() - offset);
    unsigned long read = 0;
    if (! ReadFile(file, content.data() + offset, chunk, &read, nullptr) ||
        ! read) {
      unsigned long error = GetLastError();
      CloseHandle(file);
      validation->add(read_error_message(error));
      return false;
    }
    offset += read;
  }
  CloseHandle(file);

  size_t start = content.size() >= 3 &&
                         static_cast<unsigned char>(content[0]) == 0xEF &&
                         static_cast<unsigned char>(content[1]) == 0xBB &&
                         static_cast<unsigned char>(content[2]) == 0xBF
                     ? 3
                     : 0;
  return parse_service_config(content.data() + start, content.size() - start,
                              config, validation);
}

int validate_service_config_command(int argc, wchar_t **argv) {
  if (argc != 1 || ! argv || ! argv[0]) {
    fwprintf(stderr, L"Usage: nssm validate <config.json>\n");
    return 2;
  }

  service_config_t config;
  config_validation_t validation;
  if (! load_service_config(argv[0], &config, &validation)) {
    for (const auto &error : validation.errors) {
      fwprintf(stderr, L"error: %ls\n", error.c_str());
    }
    return 1;
  }
  fwprintf(stdout, L"Configuration is valid: %ls\n",
           config.service.name.c_str());
  return 0;
}
