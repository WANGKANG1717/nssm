#ifndef UTF8_H
#define UTF8_H

void setup_utf8();
int to_utf16(const char *, wchar_t **utf16, unsigned long *);
bool encoded_newline_at(const void *, unsigned long, unsigned long,
                        unsigned long);

#endif
