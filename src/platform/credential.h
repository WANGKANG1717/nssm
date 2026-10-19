#ifndef CREDENTIAL_H
#define CREDENTIAL_H

#include <string>
#include <vector>

bool store_service_credential(const std::wstring &, const std::wstring &,
                              const std::wstring &, unsigned long *);
bool load_service_credential(const std::wstring &, const std::wstring &,
                             std::vector<wchar_t> *, unsigned long *);
bool delete_service_credential(const std::wstring &, unsigned long *);
bool service_credential_exists(const std::wstring &, const std::wstring &,
                               bool *, unsigned long *);
void secure_clear(std::vector<wchar_t> *);

#endif
