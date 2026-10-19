#ifndef SERVICE_COMMANDS_H
#define SERVICE_COMMANDS_H


int pre_remove_service(int, wchar_t **);
int control_service(unsigned long, int, wchar_t **, bool);
int control_service(unsigned long, int, wchar_t **);
int list_services(int, wchar_t **);
int service_process_tree(int, wchar_t **);
int control_services_command(unsigned long, bool, int, wchar_t **);
int is_active_command(int, wchar_t **);
int is_failed_command(int, wchar_t **);
int reset_failed_command(int, wchar_t **);
int is_enabled_command(int, wchar_t **);
int set_service_startup_command(unsigned long, int, wchar_t **);
int show_service_command(int, wchar_t **);

#endif
