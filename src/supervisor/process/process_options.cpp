#include "process_options.h"

#include <windows.h>

namespace {

struct range_t {
  int first;
  int last;
};

bool parse_cpu_number(TCHAR *text, unsigned long *number, TCHAR **end) {
  if (! text || ! number || ! end) return false;
  *number = _tcstoul(text, end, 0);
  return *end != text;
}

}  // namespace

int affinity_mask_to_string(__int64 mask, TCHAR **string) {
  if (! string) return 1;
  if (! mask) {
    *string = nullptr;
    return 0;
  }
  range_t ranges[64];
  for (auto &range : ranges) range.first = range.last = -1;

  size_t range_index = 0;
  for (size_t cpu = 0; cpu < _countof(ranges); ++cpu) {
    if (! (static_cast<unsigned long long>(mask) & (1ULL << cpu))) continue;
    if (ranges[range_index].first == -1) {
      ranges[range_index].first = ranges[range_index].last =
          static_cast<int>(cpu);
    }
    else if (ranges[range_index].last == static_cast<int>(cpu) - 1) {
      ranges[range_index].last = static_cast<int>(cpu);
    }
    else {
      ++range_index;
      ranges[range_index].first = ranges[range_index].last =
          static_cast<int>(cpu);
    }
  }

  size_t length = (range_index + 1) * 6;
  *string = static_cast<TCHAR *>(
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, length * sizeof(TCHAR)));
  if (! *string) return 2;
  size_t offset = 0;
  for (size_t index = 0; index <= range_index; ++index) {
    if (index) (*string)[offset++] = _T(',');
    int written = _sntprintf_s(*string + offset, 3, _TRUNCATE, _T("%u"),
                               ranges[index].first);
    if (written < 0) {
      HeapFree(GetProcessHeap(), 0, *string);
      *string = nullptr;
      return 3;
    }
    offset += written;
    if (ranges[index].last == ranges[index].first) continue;
    written = _sntprintf_s(
        *string + offset, 4, _TRUNCATE, _T("%c%u"),
        ranges[index].last == ranges[index].first + 1 ? _T(',') : _T('-'),
        ranges[index].last);
    if (written < 0) {
      HeapFree(GetProcessHeap(), 0, *string);
      *string = nullptr;
      return 4;
    }
    offset += written;
  }
  return 0;
}

int affinity_string_to_mask(TCHAR *string, __int64 *mask) {
  if (! mask) return 1;
  *mask = 0;
  if (! string) return 0;

  range_t ranges[64];
  for (auto &range : ranges) range.first = range.last = -1;
  TCHAR *cursor = string;
  size_t range_index = 0;
  while (*cursor) {
    unsigned long number = 0;
    TCHAR *end = nullptr;
    bool parsed = parse_cpu_number(cursor, &number, &end);
    cursor = end;
    if (! parsed || number >= _countof(ranges)) return 2;
    ranges[range_index].first = ranges[range_index].last =
        static_cast<int>(number);
    if (! *cursor) break;
    if (*cursor == _T(',')) {
      if (! cursor[1]) return 3;
      if (range_index + 1 >= _countof(ranges)) return 2;
      ++range_index;
      ++cursor;
      continue;
    }
    if (*cursor != _T('-') || ! *(++cursor)) return 3;
    parsed = parse_cpu_number(cursor, &number, &end);
    cursor = end;
    if (! parsed || number >= _countof(ranges) ||
        (*cursor && *cursor != _T(','))) return 3;
    ranges[range_index].last = static_cast<int>(number);
    if (*cursor) {
      if (! cursor[1]) return 3;
      if (range_index + 1 >= _countof(ranges)) return 2;
      ++range_index;
      ++cursor;
    }
  }
  for (size_t index = 0; index <= range_index; ++index) {
    if (ranges[index].last < ranges[index].first) return 3;
    for (int cpu = ranges[index].first; cpu <= ranges[index].last; ++cpu) {
      *mask |= static_cast<__int64>(1ULL << static_cast<unsigned int>(cpu));
    }
  }
  return 0;
}

unsigned long priority_mask() {
  return REALTIME_PRIORITY_CLASS | HIGH_PRIORITY_CLASS |
         ABOVE_NORMAL_PRIORITY_CLASS | NORMAL_PRIORITY_CLASS |
         BELOW_NORMAL_PRIORITY_CLASS | IDLE_PRIORITY_CLASS;
}

int priority_constant_to_index(unsigned long constant) {
  switch (constant & priority_mask()) {
    case REALTIME_PRIORITY_CLASS: return NSSM_REALTIME_PRIORITY;
    case HIGH_PRIORITY_CLASS: return NSSM_HIGH_PRIORITY;
    case ABOVE_NORMAL_PRIORITY_CLASS: return NSSM_ABOVE_NORMAL_PRIORITY;
    case BELOW_NORMAL_PRIORITY_CLASS: return NSSM_BELOW_NORMAL_PRIORITY;
    case IDLE_PRIORITY_CLASS: return NSSM_IDLE_PRIORITY;
    default: return NSSM_NORMAL_PRIORITY;
  }
}

unsigned long priority_index_to_constant(int index) {
  switch (index) {
    case NSSM_REALTIME_PRIORITY: return REALTIME_PRIORITY_CLASS;
    case NSSM_HIGH_PRIORITY: return HIGH_PRIORITY_CLASS;
    case NSSM_ABOVE_NORMAL_PRIORITY: return ABOVE_NORMAL_PRIORITY_CLASS;
    case NSSM_BELOW_NORMAL_PRIORITY: return BELOW_NORMAL_PRIORITY_CLASS;
    case NSSM_IDLE_PRIORITY: return IDLE_PRIORITY_CLASS;
    default: return NORMAL_PRIORITY_CLASS;
  }
}
