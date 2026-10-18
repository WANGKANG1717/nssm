#ifndef NSSM_SECURITY_H
#define NSSM_SECURITY_H

#include <windows.h>

#include <string>

bool check_secure_path(const std::wstring &, bool, std::wstring *);
bool check_secure_file_handle(HANDLE, std::wstring *);
bool is_process_administrator();

#endif
