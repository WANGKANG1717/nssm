#ifndef SVP_CLI_STATUS_H
#define SVP_CLI_STATUS_H

const wchar_t *service_state_text(unsigned long state);
int inspect_service_status_command(int argc, wchar_t **argv);

#endif
