#ifndef SERVICE_COMMANDS_H
#define SERVICE_COMMANDS_H

#include <tchar.h>

int pre_remove_service(int, TCHAR **);
int control_service(unsigned long, int, TCHAR **, bool);
int control_service(unsigned long, int, TCHAR **);
int list_nssm_services(int, TCHAR **);
int service_process_tree(int, TCHAR **);

#endif
