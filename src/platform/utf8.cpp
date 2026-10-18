#include "utf8.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>

void setup_utf8() {
  _setmode(_fileno(stdout), _O_U8TEXT);
  _setmode(_fileno(stderr), _O_U8TEXT);
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

bool encoded_newline_at(const void *address, unsigned long offset,
                        unsigned long length, unsigned long character_size) {
  if (! address) return false;
  const auto *bytes = static_cast<const unsigned char *>(address);
  if (character_size == sizeof(wchar_t)) {
    return offset + sizeof(wchar_t) <= length && bytes[offset] == '\n' &&
           bytes[offset + 1] == 0;
  }
  return character_size == sizeof(char) && offset < length &&
         bytes[offset] == '\n';
}
