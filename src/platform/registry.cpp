#include "nssm.h"
#include "win32_handle.h"

int create_messages() {
  unique_registry_key key;

  TCHAR registry[KEY_LENGTH];
  if (_sntprintf_s(registry, _countof(registry), _TRUNCATE, _T("SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\%s"), NSSM) < 0) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OUT_OF_MEMORY, _T("eventlog registry"), _T("create_messages()"), 0);
    return 1;
  }

  long error = RegCreateKeyEx(HKEY_LOCAL_MACHINE, registry, 0, 0,
                              REG_OPTION_NON_VOLATILE, KEY_WRITE, 0, key.put(), 0);
  if (error != ERROR_SUCCESS) {
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_OPENKEY_FAILED, registry, error_string(error), 0);
    return 2;
  }

  /* Get path of this program */
  const TCHAR *path = nssm_unquoted_imagepath();

  /* Try to register the module but don't worry so much on failure */
  RegSetValueEx(key.get(), _T("EventMessageFile"), 0, REG_SZ, (const unsigned char *) path, (unsigned long) (_tcslen(path) +  1) * sizeof(TCHAR));
  unsigned long types = EVENTLOG_INFORMATION_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_ERROR_TYPE;
  RegSetValueEx(key.get(), _T("TypesSupported"), 0, REG_DWORD, (const unsigned char *) &types, sizeof(types));

  return 0;
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
