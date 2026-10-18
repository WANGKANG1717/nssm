#ifndef UTF8_H
#define UTF8_H

void setup_utf8();
void unsetup_utf8();
int to_utf16(const wchar_t *, wchar_t **utf16, unsigned long *);
int to_utf16(const char *, wchar_t **utf16, unsigned long *);

#endif
