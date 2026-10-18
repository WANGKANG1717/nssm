#ifndef REGISTRY_H
#define REGISTRY_H

#define NSSM_REGISTRY_GROUPS _T("SYSTEM\\CurrentControlSet\\Control\\ServiceGroupOrder")
#define NSSM_REG_GROUPS _T("List")
#define NSSM_REG_STOP_METHOD_SKIP _T("AppStopMethodSkip")
#define NSSM_REG_STDOUT _T("AppStdout")
#define NSSM_REG_STDERR _T("AppStderr")

int create_messages();
int copy_double_null(TCHAR *, unsigned long, TCHAR **);
int append_to_double_null(TCHAR *, unsigned long, TCHAR **, unsigned long *, TCHAR *, size_t, bool);
int remove_from_double_null(TCHAR *, unsigned long, TCHAR **, unsigned long *, TCHAR *, size_t, bool);

#endif
