#ifndef SERVICE_COMMANDS_H
#define SERVICE_COMMANDS_H


int pre_remove_service(int, wchar_t **);
int control_service(unsigned long, int, wchar_t **, bool);
int control_service(unsigned long, int, wchar_t **);
int list_nssm_services(int, wchar_t **);
int service_process_tree(int, wchar_t **);

#endif
