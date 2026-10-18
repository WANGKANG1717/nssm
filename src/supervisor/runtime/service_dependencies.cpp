#include "nssm.h"
#include "event.h"
#include "messages.h"
#include "registry.h"
#include "service.h"
#include "win32_handle.h"

#include <vector>

namespace {

bool load_service_groups(std::vector<TCHAR> *groups) {
  unique_registry_key key;
  long result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, NSSM_REGISTRY_GROUPS, 0,
                             KEY_READ, key.put());
  if (result != ERROR_SUCCESS) {
    _ftprintf(stderr, _T("%s: %s\n"), NSSM_REGISTRY_GROUPS,
              error_string(result));
    return false;
  }

  unsigned long type = 0;
  unsigned long bytes = 0;
  result = RegQueryValueEx(key.get(), NSSM_REG_GROUPS, nullptr, &type, nullptr,
                           &bytes);
  if (result == ERROR_FILE_NOT_FOUND) {
    groups->assign(2, _T('\0'));
    return true;
  }
  if (result != ERROR_SUCCESS || type != REG_MULTI_SZ ||
      bytes < 2 * sizeof(TCHAR)) {
    _ftprintf(stderr, _T("%s\\%s: %s\n"), NSSM_REGISTRY_GROUPS,
              NSSM_REG_GROUPS,
              error_string(result == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                                    : result));
    return false;
  }

  groups->assign(bytes / sizeof(TCHAR) + 1, _T('\0'));
  result = RegQueryValueEx(
      key.get(), NSSM_REG_GROUPS, nullptr, &type,
      reinterpret_cast<unsigned char *>(groups->data()), &bytes);
  if (result != ERROR_SUCCESS) {
    _ftprintf(stderr, _T("%s\\%s: %s\n"), NSSM_REGISTRY_GROUPS,
              NSSM_REG_GROUPS, error_string(result));
    return false;
  }
  groups->back() = _T('\0');
  return true;
}

const TCHAR *find_service_group(const std::vector<TCHAR> &groups,
                                const TCHAR *requested) {
  for (const TCHAR *group = groups.data(); *group;
       group += _tcslen(group) + 1) {
    if (str_equiv(group, requested)) return group;
  }
  return nullptr;
}

void append_dependency(std::vector<TCHAR> *dependencies,
                       const TCHAR *dependency) {
  dependencies->insert(dependencies->end(), dependency,
                       dependency + _tcslen(dependency) + 1);
}

}  // namespace

/* WILL NOT allocate a new string if the identifier is already present. */
int prepend_service_group_identifier(TCHAR *group, TCHAR **canon) {
  if (! group || ! group[0] || group[0] == SC_GROUP_IDENTIFIER) {
    *canon = group;
    return 0;
  }

  size_t len = _tcslen(group) + 1;
  *canon = static_cast<TCHAR *>(
      HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(TCHAR)));
  if (! *canon) {
    print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, _T("canon"),
                  _T("prepend_service_group_identifier()"));
    return 1;
  }

  TCHAR *destination = *canon;
  *destination++ = SC_GROUP_IDENTIFIER;
  memmove(destination, group, len * sizeof(TCHAR));
  (*canon)[len] = _T('\0');
  return 0;
}

int append_to_dependencies(TCHAR *dependencies, unsigned long dependencieslen,
                           TCHAR *string, TCHAR **newdependencies,
                           unsigned long *newlen, int type) {
  *newlen = 0;
  TCHAR *canonical = nullptr;
  if (type == DEPENDENCY_GROUPS) {
    if (prepend_service_group_identifier(string, &canonical)) return 1;
  }
  else canonical = string;
  int result = append_to_double_null(dependencies, dependencieslen,
                                     newdependencies, newlen, canonical, 0,
                                     false);
  if (canonical && canonical != string) HeapFree(GetProcessHeap(), 0, canonical);
  return result;
}

int remove_from_dependencies(TCHAR *dependencies,
                             unsigned long dependencieslen, TCHAR *string,
                             TCHAR **newdependencies, unsigned long *newlen,
                             int type) {
  *newlen = 0;
  TCHAR *canonical = nullptr;
  if (type == DEPENDENCY_GROUPS) {
    if (prepend_service_group_identifier(string, &canonical)) return 1;
  }
  else canonical = string;
  int result = remove_from_double_null(dependencies, dependencieslen,
                                       newdependencies, newlen, canonical, 0,
                                       false);
  if (canonical && canonical != string) HeapFree(GetProcessHeap(), 0, canonical);
  return result;
}

int set_service_dependencies(const TCHAR *service_name,
                             SC_HANDLE service_handle, TCHAR *buffer) {
  (void) service_name;
  std::vector<TCHAR> dependencies;
  dependencies.reserve(2);

  if (buffer && buffer[0]) {
    unique_service_handle manager(open_service_manager(
        SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
    if (! manager) {
      print_message(stderr, NSSM_MESSAGE_OPEN_SERVICE_MANAGER_FAILED);
      return 1;
    }

    bool has_groups = false;
    for (TCHAR *entry = buffer; *entry; entry += _tcslen(entry) + 1) {
      if (*entry == SC_GROUP_IDENTIFIER) {
        has_groups = true;
        break;
      }
    }
    std::vector<TCHAR> groups;
    if (has_groups && ! load_service_groups(&groups)) return 2;

    for (TCHAR *entry = buffer; *entry; entry += _tcslen(entry) + 1) {
      TCHAR canonical[SERVICE_NAME_LENGTH] = {};
      if (*entry == SC_GROUP_IDENTIFIER) {
        const TCHAR *group = entry + 1;
        const TCHAR *match = *group ? find_service_group(groups, group) : nullptr;
        if (! match || _sntprintf_s(canonical, _countof(canonical), _TRUNCATE,
                                    _T("%c%s"), SC_GROUP_IDENTIFIER,
                                    match) < 0) {
          _ftprintf(stderr, _T("%s: %s\n"), entry,
                    error_string(ERROR_SERVICE_DEPENDENCY_DELETED));
          return 5;
        }
      }
      else {
        unique_service_handle dependency(open_service(
            manager.get(), entry, SERVICE_QUERY_STATUS, canonical,
            _countof(canonical)));
        if (! dependency) {
          _ftprintf(stderr, _T("%s: %s\n"), entry,
                    error_string(ERROR_SERVICE_DEPENDENCY_DELETED));
          return 5;
        }
      }
      append_dependency(&dependencies, canonical);
    }
  }

  dependencies.push_back(_T('\0'));
  if (dependencies.size() == 1) dependencies.push_back(_T('\0'));
  if (! ChangeServiceConfig(service_handle, SERVICE_NO_CHANGE,
                            SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, nullptr,
                            nullptr, nullptr, dependencies.data(), nullptr,
                            nullptr, nullptr)) {
    print_message(stderr, NSSM_MESSAGE_CHANGESERVICECONFIG_FAILED,
                  error_string(GetLastError()));
    return -1;
  }
  return 0;
}

int get_service_dependencies(const TCHAR *service_name,
                             SC_HANDLE service_handle, TCHAR **buffer,
                             unsigned long *bufsize, int type) {
  if (! buffer) return 1;
  if (! bufsize) return 2;
  *buffer = nullptr;
  *bufsize = 0;

  QUERY_SERVICE_CONFIG *config =
      query_service_config(service_name, service_handle);
  if (! config) return 3;

  if (! config->lpDependencies || ! config->lpDependencies[0]) {
    HeapFree(GetProcessHeap(), 0, config);
    return 0;
  }

  std::vector<TCHAR> selected;
  for (TCHAR *entry = config->lpDependencies; *entry;
       entry += _tcslen(entry) + 1) {
    bool group = *entry == SC_GROUP_IDENTIFIER;
    if ((group && (type & DEPENDENCY_GROUPS)) ||
        (! group && (type & DEPENDENCY_SERVICES))) {
      append_dependency(&selected, entry);
    }
  }
  HeapFree(GetProcessHeap(), 0, config);

  if (selected.empty()) return 0;
  selected.push_back(_T('\0'));
  *bufsize = static_cast<unsigned long>(selected.size());
  *buffer = static_cast<TCHAR *>(
      HeapAlloc(GetProcessHeap(), 0, selected.size() * sizeof(TCHAR)));
  if (! *buffer) {
    *bufsize = 0;
    print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, _T("lpDependencies"),
                  _T("get_service_dependencies()"));
    return 4;
  }
  memmove(*buffer, selected.data(), selected.size() * sizeof(TCHAR));
  return 0;
}

int get_service_dependencies(const TCHAR *service_name,
                             SC_HANDLE service_handle, TCHAR **buffer,
                             unsigned long *bufsize) {
  return get_service_dependencies(service_name, service_handle, buffer, bufsize,
                                  DEPENDENCY_ALL);
}
