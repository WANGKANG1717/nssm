#ifndef EVENT_H
#define EVENT_H

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdio>

wchar_t *error_string(unsigned long);
void log_event_strings(unsigned short, unsigned long,
                       const wchar_t *const *, size_t);

namespace event_detail {

inline const wchar_t *argument(const wchar_t *value) noexcept {
  return value;
}

inline const wchar_t *argument(int sentinel) noexcept {
  return sentinel == 0 ? nullptr : L"";
}

}  // namespace event_detail

template <typename... Arguments>
void log_event(unsigned short type, unsigned long id, Arguments... arguments) {
  static_assert(sizeof...(arguments) <= 16);
  const std::array<const wchar_t *, sizeof...(arguments)> strings = {
      event_detail::argument(arguments)...};
  size_t count = 0;
  while (count < strings.size() && strings[count]) ++count;
  log_event_strings(type, id, strings.data(), count);
}

void print_message(FILE *, unsigned long, ...);

#endif
