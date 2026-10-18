#include "nssm.h"

static int service_registry_path(const TCHAR *service_name, const TCHAR *sub, TCHAR *buffer, unsigned long buflen) {
  if (sub) return _sntprintf_s(buffer, buflen, _TRUNCATE, NSSM_REGISTRY _T("\\") NSSM_REG_PARAMETERS _T("\\%s"), service_name, sub);
  return _sntprintf_s(buffer, buflen, _TRUNCATE, NSSM_REGISTRY _T("\\") NSSM_REG_PARAMETERS, service_name);
}

static long open_registry_key(const TCHAR *registry, REGSAM sam, HKEY *key, bool must_exist) {
  long error;

  if (sam & KEY_SET_VALUE) {
    error = RegCreateKeyEx(HKEY_LOCAL_MACHINE, registry, 0, 0, REG_OPTION_NON_VOLATILE, sam, 0, key, 0);
    if (error != ERROR_SUCCESS) {
      *key = 0;
      log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED, registry, error_string(error), 0);
      return error;
    }
  }
  else {
    error = RegOpenKeyEx(HKEY_LOCAL_MACHINE, registry, 0, sam, key);
    if (error != ERROR_SUCCESS) {
      *key = 0;
      if (error != ERROR_FILE_NOT_FOUND || must_exist) log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED, registry, error_string(error), 0);
    }
  }

  return error;
}

int create_messages() {
  HKEY key;

  TCHAR registry[KEY_LENGTH];
  if (_sntprintf_s(registry, _countof(registry), _TRUNCATE, _T("SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\%s"), NSSM) < 0) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("eventlog registry"), _T("create_messages()"), 0);
    return 1;
  }

  long error = RegCreateKeyEx(HKEY_LOCAL_MACHINE, registry, 0, 0, REG_OPTION_NON_VOLATILE, KEY_WRITE, 0, &key, 0);
  if (error != ERROR_SUCCESS) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED, registry, error_string(error), 0);
    return 2;
  }

  /* Get path of this program */
  const TCHAR *path = nssm_unquoted_imagepath();

  /* Try to register the module but don't worry so much on failure */
  RegSetValueEx(key, _T("EventMessageFile"), 0, REG_SZ, (const unsigned char *) path, (unsigned long) (_tcslen(path) +  1) * sizeof(TCHAR));
  unsigned long types = EVENTLOG_INFORMATION_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_ERROR_TYPE;
  RegSetValueEx(key, _T("TypesSupported"), 0, REG_DWORD, (const unsigned char *) &types, sizeof(types));

  RegCloseKey(key);
  return 0;
}

int get_string(HKEY key, TCHAR *value, TCHAR *data, unsigned long datalen, bool expand, bool sanitise, bool must_exist) {
  TCHAR *buffer = (TCHAR *) HeapAlloc(GetProcessHeap(), 0, datalen);
  if (! buffer) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, value, _T("get_string()"), 0);
    return 1;
  }

  ZeroMemory(data, datalen);

  unsigned long type = REG_EXPAND_SZ;
  unsigned long buflen = datalen;

  unsigned long ret = RegQueryValueEx(key, value, 0, &type, (unsigned char *) buffer, &buflen);
  if (ret != ERROR_SUCCESS) {
    HeapFree(GetProcessHeap(), 0, buffer);

    if (ret == ERROR_FILE_NOT_FOUND) {
      if (! must_exist) return 0;
    }

    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_QUERYVALUE_FAILED, value, error_string(ret), 0);
    return 2;
  }

  /* Paths aren't allowed to contain quotes. */
  if (sanitise) PathUnquoteSpaces(buffer);

  /* Do we want to expand the string? */
  if (! expand) {
    if (type == REG_EXPAND_SZ) type = REG_SZ;
  }

  /* Technically we shouldn't expand environment strings from REG_SZ values */
  if (type != REG_EXPAND_SZ) {
    memmove(data, buffer, buflen);
    HeapFree(GetProcessHeap(), 0, buffer);
    return 0;
  }

  ret = ExpandEnvironmentStrings((TCHAR *) buffer, data, datalen);
  if (! ret || ret > datalen) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_EXPANDENVIRONMENTSTRINGS_FAILED, buffer, error_string(GetLastError()), 0);
    HeapFree(GetProcessHeap(), 0, buffer);
    return 3;
  }

  HeapFree(GetProcessHeap(), 0, buffer);
  return 0;
}

int expand_parameter(HKEY key, TCHAR *value, TCHAR *data, unsigned long datalen, bool sanitise, bool must_exist) {
  return get_string(key, value, data, datalen, true, sanitise, must_exist);
}

/* Copy a block. */
int copy_double_null(TCHAR *dn, unsigned long dnlen, TCHAR **newdn) {
  if (! newdn) return 1;

  *newdn = 0;
  if (! dn) return 0;

  *newdn = (TCHAR *) HeapAlloc(GetProcessHeap(), 0, dnlen * sizeof(TCHAR));
  if (! *newdn) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("dn"), _T("copy_double_null()"), 0);
    return 2;
  }

  memmove(*newdn, dn, dnlen * sizeof(TCHAR));
  return 0;
}

/*
  Create a new block with all the strings of the first block plus a new string.
  The new string may be specified as <key> <delimiter> <value> and the keylen
  gives the offset into the string to compare against existing entries.
  If the key is already present its value will be overwritten in place.
  If the key is blank or empty the new block will still be allocated and have
  non-zero length.
*/
int append_to_double_null(TCHAR *dn, unsigned long dnlen, TCHAR **newdn, unsigned long *newlen, TCHAR *append, size_t keylen, bool case_sensitive) {
  if (! append || ! append[0]) return copy_double_null(dn, dnlen, newdn);
  size_t appendlen = _tcslen(append);
  int (*fn)(const TCHAR *, const TCHAR *, size_t) = (case_sensitive) ? _tcsncmp : _tcsnicmp;

  /* Identify the key, if any, or treat the whole string as the key. */
  TCHAR *key = 0;
  if (! keylen || keylen > appendlen) keylen = appendlen;
  key = (TCHAR *) HeapAlloc(GetProcessHeap(), 0, (keylen + 1) * sizeof(TCHAR));
  if (! key) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("key"), _T("append_to_double_null()"), 0);
    return 1;
  }
  memmove(key, append, keylen * sizeof(TCHAR));
  key[keylen] = _T('\0');

  /* Find the length of the block not including any existing key. */
  size_t len = 0;
  TCHAR *s;
  for (s = dn; *s; s++) {
    if (fn(s, key, keylen)) len += _tcslen(s) + 1;
    for ( ; *s; s++);
  }

  /* Account for new entry. */
  len += _tcslen(append) + 1;

  /* Account for trailing NULL. */
  len++;

  /* Allocate a new block. */
  *newdn = (TCHAR *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len * sizeof(TCHAR));
  if (! *newdn) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("newdn"), _T("append_to_double_null()"), 0);
    HeapFree(GetProcessHeap(), 0, key);
    return 2;
  }

  /* Copy existing entries.*/
  *newlen = (unsigned long) len;
  TCHAR *t = *newdn;
  TCHAR *u;
  bool replaced = false;
  for (s = dn; *s; s++) {
    if (fn(s, key, keylen)) u = s;
    else {
      u = append;
      replaced = true;
    }
    len = _tcslen(u) + 1;
    memmove(t, u, len * sizeof(TCHAR));
    t += len;
    for ( ; *s; s++);
  }

  /* Add the entry if it wasn't already replaced.  The buffer was zeroed. */
  if (! replaced) memmove(t, append, _tcslen(append) * sizeof(TCHAR));

  HeapFree(GetProcessHeap(), 0, key);
  return 0;
}

/*
  Create a new block with all the string of the first block minus the given
  string.
  The keylen parameter gives the offset into the string to compare against
  existing entries.  If a substring of existing value matches the string to
  the given length it will be removed.
  If the last entry is removed the new block will still be allocated and
  have non-zero length.
*/
int remove_from_double_null(TCHAR *dn, unsigned long dnlen, TCHAR **newdn, unsigned long *newlen, TCHAR *remove, size_t keylen, bool case_sensitive) {
  if (! remove || !remove[0]) return copy_double_null(dn, dnlen, newdn);
  size_t removelen = _tcslen(remove);
  int (*fn)(const TCHAR *, const TCHAR *, size_t) = (case_sensitive) ? _tcsncmp : _tcsnicmp;

  /* Identify the key, if any, or treat the whole string as the key. */
  TCHAR *key = 0;
  if (! keylen || keylen > removelen) keylen = removelen;
  key = (TCHAR *) HeapAlloc(GetProcessHeap(), 0, (keylen + 1) * sizeof(TCHAR));
  if (! key) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("key"), _T("remove_from_double_null()"), 0);
    return 1;
  }
  memmove(key, remove, keylen * sizeof(TCHAR));
  key[keylen] = _T('\0');

  /* Find the length of the block not including any existing key. */
  size_t len = 0;
  TCHAR *s;
  for (s = dn; *s; s++) {
    if (fn(s, key, keylen)) len += _tcslen(s) + 1;
    for ( ; *s; s++);
  }

  /* Account for trailing NULL. */
  if (++len < 2) len = 2;

  /* Allocate a new block. */
  *newdn = (TCHAR *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len * sizeof(TCHAR));
  if (! *newdn) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("newdn"), _T("remove_from_double_null()"), 0);
    HeapFree(GetProcessHeap(), 0, key);
    return 2;
  }

  /* Copy existing entries.*/
  *newlen = (unsigned long) len;
  TCHAR *t = *newdn;
  for (s = dn; *s; s++) {
    if (fn(s, key, keylen)) {
      len = _tcslen(s) + 1;
      memmove(t, s, len * sizeof(TCHAR));
      t += len;
    }
    for ( ; *s; s++);
  }

  HeapFree(GetProcessHeap(), 0, key);
  return 0;
}

/* Open a subkey of the service Services\<service_name>\<sub>. */
long open_registry(const TCHAR *service_name, const TCHAR *sub, REGSAM sam, HKEY *key, bool must_exist) {
  if (key) *key = 0;
  /* Get registry */
  TCHAR registry[KEY_LENGTH];
  if (service_registry_path(service_name, sub, registry, _countof(registry)) < 0) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, NSSM_REGISTRY, _T("open_registry()"), 0);
    return ERROR_OUTOFMEMORY;
  }

  return open_registry_key(registry, sam, key, must_exist);
}

int get_hook(const TCHAR *service_name, const TCHAR *hook_event, const TCHAR *hook_action, TCHAR *buffer, unsigned long buflen) {
  /* Try to open the registry */
  TCHAR registry[KEY_LENGTH];
  if (_sntprintf_s(registry, _countof(registry), _TRUNCATE, _T("%s\\%s"), NSSM_REG_HOOK, hook_event) < 0) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("hook registry"), _T("get_hook()"), 0);
    return 1;
  }
  HKEY key;
  long error = open_registry(service_name, registry, KEY_READ, &key, false);
  if (! key) {
    if (error == ERROR_FILE_NOT_FOUND) {
      ZeroMemory(buffer, buflen);
      return 0;
    }
    return 1;
  }

  int ret = expand_parameter(key, (TCHAR *) hook_action, buffer, buflen, true, false);

  /* Close registry */
  RegCloseKey(key);

  return ret;
}
