#ifndef NSSM_SECURITY_H
#define NSSM_SECURITY_H

#include <string>

bool check_secure_path(const std::wstring &, bool, std::wstring *);
bool is_process_administrator();

#endif
