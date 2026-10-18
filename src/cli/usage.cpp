#include "usage.h"

#include "event.h"
#include "messages.h"
#include "version.h"

#include <cstdio>
#include <tchar.h>

int usage(int result) {
  print_message(stderr, NSSM_MESSAGE_USAGE, NSSM_VERSION,
                NSSM_CONFIGURATION, NSSM_DATE);
  return result;
}
