#include "child_process.h"

#include <cwctype>
#include <map>

namespace {

struct environment_name_less {
  bool operator()(const std::wstring &left, const std::wstring &right) const {
    size_t common = left.size() < right.size() ? left.size() : right.size();
    for (size_t i = 0; i < common; ++i) {
      wchar_t a = static_cast<wchar_t>(std::towupper(left[i]));
      wchar_t b = static_cast<wchar_t>(std::towupper(right[i]));
      if (a < b) return true;
      if (a > b) return false;
    }
    return left.size() < right.size();
  }
};

using environment_map_t =
    std::map<std::wstring, std::wstring, environment_name_less>;

bool split_environment_entry(const std::wstring &entry,
                             environment_entry_t *result,
                             std::wstring *error) {
  if (! result) return false;

  /* Drive-current-directory entries have the form =C:=C:\\path. */
  size_t separator = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
  if (separator == std::wstring::npos || separator == 0) {
    if (error) *error = L"Environment entry must have the form NAME=VALUE";
    return false;
  }

  result->first = entry.substr(0, separator);
  result->second = entry.substr(separator + 1);
  return true;
}

bool merge_environment_block(const wchar_t *block,
                             environment_map_t *values,
                             std::wstring *error) {
  if (! block) return true;
  if (! values) return false;

  std::vector<environment_entry_t> entries;
  if (! parse_environment_block(block, &entries, error)) return false;
  for (const auto &entry : entries) values->insert_or_assign(entry.first, entry.second);
  return true;
}

void serialize_environment(const environment_map_t &values,
                           std::vector<wchar_t> *environment) {
  environment->clear();
  for (const auto &entry : values) {
    environment->insert(environment->end(), entry.first.begin(), entry.first.end());
    environment->push_back(L'=');
    environment->insert(environment->end(), entry.second.begin(), entry.second.end());
    environment->push_back(L'\0');
  }

  /* An empty Windows environment block still contains two NUL characters. */
  if (values.empty()) environment->push_back(L'\0');
  environment->push_back(L'\0');
}

}  // namespace

std::wstring quote_windows_argument(const std::wstring &argument) {
  if (! argument.empty() &&
      argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }

  std::wstring quoted;
  quoted.push_back(L'"');
  size_t backslashes = 0;
  for (wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
    }
    else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(character);
    }
    backslashes = 0;
  }
  /* Backslashes before the closing quote must be doubled. */
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

bool build_child_command_line(const std::wstring &executable,
                              const std::wstring &arguments,
                              std::wstring *command_line,
                              std::wstring *error) {
  if (! command_line) return false;
  command_line->clear();

  if (executable.empty()) {
    if (error) *error = L"Executable path must not be empty";
    return false;
  }
  if (executable.find(L'"') != std::wstring::npos) {
    if (error) *error = L"Executable path must not contain a quote character";
    return false;
  }

  command_line->reserve(executable.size() + arguments.size() + 4);
  command_line->push_back(L'"');
  command_line->append(executable);
  command_line->push_back(L'"');
  if (! arguments.empty()) {
    command_line->push_back(L' ');
    command_line->append(arguments);
  }
  /* CreateProcessW limits lpCommandLine to 32767 characters including NUL. */
  if (command_line->size() >= 32767) {
    command_line->clear();
    if (error) *error = L"Command line exceeds the CreateProcessW limit";
    return false;
  }
  return true;
}

bool build_child_command_line(const std::wstring &executable,
                              const std::vector<std::wstring> &arguments,
                              std::wstring *command_line,
                              std::wstring *error) {
  std::wstring serialized;
  for (const auto &argument : arguments) {
    if (! serialized.empty()) serialized.push_back(L' ');
    serialized.append(quote_windows_argument(argument));
  }
  return build_child_command_line(executable, serialized, command_line, error);
}

bool parse_environment_block(const wchar_t *block,
                             std::vector<environment_entry_t> *entries,
                             std::wstring *error) {
  if (! entries) return false;
  entries->clear();
  if (! block) return true;

  for (const wchar_t *cursor = block; *cursor; cursor += wcslen(cursor) + 1) {
    environment_entry_t entry;
    if (! split_environment_entry(cursor, &entry, error)) return false;
    entries->push_back(std::move(entry));
  }
  return true;
}

bool build_child_environment(const wchar_t *base,
                             const wchar_t *replacement,
                             const wchar_t *extra,
                             std::vector<wchar_t> *environment,
                             std::wstring *error) {
  if (! environment) return false;

  environment_map_t values;
  if (! merge_environment_block(replacement ? replacement : base, &values, error)) {
    return false;
  }
  if (! merge_environment_block(extra, &values, error)) return false;

  serialize_environment(values, environment);
  return true;
}

bool extend_child_environment(
    const wchar_t *base,
    const std::vector<environment_entry_t> &extra,
    std::vector<wchar_t> *environment,
    std::wstring *error) {
  if (! environment) return false;

  environment_map_t values;
  if (! merge_environment_block(base, &values, error)) return false;
  for (const auto &entry : extra) {
    if (entry.first.empty() || entry.first.find(L'=') != std::wstring::npos) {
      if (error) *error = L"Environment variable name is invalid";
      return false;
    }
    values.insert_or_assign(entry.first, entry.second);
  }

  serialize_environment(values, environment);
  return true;
}
