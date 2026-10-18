#ifndef CHILD_PROCESS_H
#define CHILD_PROCESS_H

#include <windows.h>
#include <tchar.h>

#include <string>
#include <utility>
#include <vector>

using environment_entry_t = std::pair<std::wstring, std::wstring>;

/*
  Build the exact command line passed to CreateProcessW().  The executable is
  also supplied separately through lpApplicationName, so Windows never has to
  guess where the executable path ends.
*/
bool build_child_command_line(const std::wstring &executable,
                              const std::wstring &arguments,
                              std::wstring *command_line,
                              std::wstring *error);
bool build_child_command_line(const std::wstring &executable,
                              const std::vector<std::wstring> &arguments,
                              std::wstring *command_line,
                              std::wstring *error);

/* Apply the quoting rules used by CommandLineToArgvW and the Microsoft CRT. */
std::wstring quote_windows_argument(const std::wstring &argument);
std::wstring join_windows_arguments(const std::vector<std::wstring> &arguments);

/* Parse a Windows double-NUL-terminated environment block. */
bool parse_environment_block(const wchar_t *block,
                             std::vector<environment_entry_t> *entries,
                             std::wstring *error);

/*
  Build a sorted Unicode environment block without mutating the wrapper's
  process environment.

  If replacement is non-null it replaces base.  Extra values are then merged
  case-insensitively, matching Windows environment variable semantics.
*/
bool build_child_environment(const wchar_t *base,
                             const wchar_t *replacement,
                             const wchar_t *extra,
                             std::vector<wchar_t> *environment,
                             std::wstring *error);

/* Merge explicit variables into an existing environment block. */
bool extend_child_environment(
    const wchar_t *base,
    const std::vector<environment_entry_t> &extra,
    std::vector<wchar_t> *environment,
    std::wstring *error);

#endif
