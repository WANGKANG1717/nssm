#include "usage.h"

#include "event.h"
#include "messages.h"
#include "version.h"

#include <cstdio>

int usage(int result) {
  print_message(result ? stderr : stdout, SVP_MESSAGE_USAGE, SVP_VERSION,
                SVP_CONFIGURATION, SVP_DATE);
  return result;
}
