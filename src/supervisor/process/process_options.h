#ifndef NSSM_PROCESS_OPTIONS_H
#define NSSM_PROCESS_OPTIONS_H

#include <tchar.h>

enum process_priority_index_t {
  NSSM_REALTIME_PRIORITY = 0,
  NSSM_HIGH_PRIORITY,
  NSSM_ABOVE_NORMAL_PRIORITY,
  NSSM_NORMAL_PRIORITY,
  NSSM_BELOW_NORMAL_PRIORITY,
  NSSM_IDLE_PRIORITY,
};

int affinity_mask_to_string(__int64 mask, TCHAR **string);
int affinity_string_to_mask(TCHAR *string, __int64 *mask);
unsigned long priority_mask();
int priority_constant_to_index(unsigned long constant);
unsigned long priority_index_to_constant(int index);

#endif
