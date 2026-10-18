#ifndef SERVICE_OPERATIONS_H
#define SERVICE_OPERATIONS_H

#include <windows.h>

#include <string>

struct service_control_result_t {
  std::wstring service_name;
  SERVICE_STATUS status = {};
  unsigned long windows_error = ERROR_SUCCESS;
  int wait_response = 0;
};

bool execute_service_control(unsigned long, const std::wstring &,
                             service_control_result_t *);

#endif
