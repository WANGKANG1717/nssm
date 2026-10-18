#ifndef REGISTRY_H
#define REGISTRY_H

#define NSSM_REGISTRY _T("SYSTEM\\CurrentControlSet\\Services\\%s")
#define NSSM_REG_PARAMETERS _T("Parameters")
#define NSSM_REGISTRY_GROUPS _T("SYSTEM\\CurrentControlSet\\Control\\ServiceGroupOrder")
#define NSSM_REG_GROUPS _T("List")
#define NSSM_REG_STOP_METHOD_SKIP _T("AppStopMethodSkip")
#define NSSM_REG_STDOUT _T("AppStdout")
#define NSSM_REG_STDERR _T("AppStderr")
#define NSSM_REG_HOOK _T("AppEvents")

long open_registry(const TCHAR *, const TCHAR *, REGSAM sam, HKEY *, bool);
int create_messages();
int get_string(HKEY, TCHAR *, TCHAR *, unsigned long, bool, bool, bool);
int expand_parameter(HKEY, TCHAR *, TCHAR *, unsigned long, bool, bool);
int copy_double_null(TCHAR *, unsigned long, TCHAR **);
int append_to_double_null(TCHAR *, unsigned long, TCHAR **, unsigned long *, TCHAR *, size_t, bool);
int remove_from_double_null(TCHAR *, unsigned long, TCHAR **, unsigned long *, TCHAR *, size_t, bool);
int get_hook(const TCHAR *, const TCHAR *, const TCHAR *, TCHAR *, unsigned long);

#endif
