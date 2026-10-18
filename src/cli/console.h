#ifndef CONSOLE_H
#define CONSOLE_H

typedef struct nssm_service_t nssm_service_t;

bool check_console();
void alloc_console(nssm_service_t *);

#endif
