#ifndef SERVICE_ENUMERATION_H
#define SERVICE_ENUMERATION_H

#include <string>
#include <vector>

bool enumerate_windows_services(bool, std::vector<std::wstring> *,
                                unsigned long *);

#endif
