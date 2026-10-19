#ifndef SERVICE_ENUMERATION_H
#define SERVICE_ENUMERATION_H

#include <string>
#include <vector>

struct service_enumeration_entry_t {
  std::wstring name;
  std::wstring display_name;
  unsigned long state = 0;
  unsigned long process_id = 0;
};

bool enumerate_windows_services(bool,
                                std::vector<service_enumeration_entry_t> *,
                                unsigned long *);

#endif
