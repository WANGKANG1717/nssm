#include "nssm.h"

static unsigned long original_console_code_page;

void setup_utf8() {
  original_console_code_page = GetConsoleOutputCP();
  SetConsoleOutputCP(CP_UTF8);
  _setmode(_fileno(stdout), _O_U8TEXT);
  _setmode(_fileno(stderr), _O_U8TEXT);
}

void unsetup_utf8() {
  if (original_console_code_page) {
    SetConsoleOutputCP(original_console_code_page);
  }
}

int to_utf16(const wchar_t *source, wchar_t **destination,
             unsigned long *length) {
  *destination = nullptr;
  if (length) *length = 0;
  if (! source) return 1;

  size_t source_length = wcslen(source);
  size_t bytes = (source_length + 1) * sizeof(wchar_t);
  *destination = static_cast<wchar_t *>(
      HeapAlloc(GetProcessHeap(), 0, bytes));
  if (! *destination) return 2;

  memcpy(*destination, source, bytes);
  if (length) *length = static_cast<unsigned long>(source_length);
  return 0;
}

int to_utf16(const char *source, wchar_t **destination,
             unsigned long *length) {
  *destination = nullptr;
  if (length) *length = 0;
  if (! source) return 1;

  int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source,
                                       -1, nullptr, 0);
  if (! characters) return 1;
  *destination = static_cast<wchar_t *>(HeapAlloc(
      GetProcessHeap(), 0, characters * sizeof(wchar_t)));
  if (! *destination) return 2;
  if (! MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, -1,
                            *destination, characters)) {
    HeapFree(GetProcessHeap(), 0, *destination);
    *destination = nullptr;
    return 3;
  }
  if (length) *length = static_cast<unsigned long>(characters - 1);
  return 0;
}

int from_utf16(const wchar_t *source, TCHAR **destination,
               unsigned long *length) {
  return to_utf16(source, destination, length);
}
