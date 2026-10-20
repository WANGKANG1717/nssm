#ifndef NSSM_PLATFORM_CONSOLE_H
#define NSSM_PLATFORM_CONSOLE_H

#include <cstddef>

bool console_attached();
void allocate_console(bool enabled);
bool write_standard_output_bytes(const void *data, size_t size,
                                 unsigned long *error = nullptr);

#endif
