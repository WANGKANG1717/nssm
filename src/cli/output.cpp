#include "output.h"
#include "string_utils.h"

#include <cwchar>

cli_arguments_t parse_cli_arguments(int argc, wchar_t **argv) {
  cli_arguments_t parsed;
  for (int index = 0; index < argc; ++index) {
    if (strings_equal_ignore_case(argv[index], L"--json")) {
      parsed.json = true;
    }
    else if (strings_equal_ignore_case(argv[index], L"--verbose")) {
      parsed.verbose = true;
    }
    else {
      parsed.positional.push_back(argv[index]);
    }
  }
  return parsed;
}

void write_json_string(FILE *stream, const std::wstring &value) {
  fputwc(L'"', stream);
  for (wchar_t character : value) {
    switch (character) {
      case L'"': fputws(L"\\\"", stream); break;
      case L'\\': fputws(L"\\\\", stream); break;
      case L'\b': fputws(L"\\b", stream); break;
      case L'\f': fputws(L"\\f", stream); break;
      case L'\n': fputws(L"\\n", stream); break;
      case L'\r': fputws(L"\\r", stream); break;
      case L'\t': fputws(L"\\t", stream); break;
      default:
        if (character < 0x20) fwprintf(stream, L"\\u%04x", character);
        else fputwc(character, stream);
    }
  }
  fputwc(L'"', stream);
}

void print_cli_error(bool json, const wchar_t *code,
                     const std::wstring &message,
                     unsigned long windows_error) {
  if (! json) {
    fwprintf(stderr, L"error: %ls", message.c_str());
    if (windows_error) fwprintf(stderr, L" (Windows error %lu)", windows_error);
    fputwc(L'\n', stderr);
    return;
  }
  fputws(L"{\"ok\":false,\"error\":{\"code\":", stdout);
  write_json_string(stdout, code ? code : L"error");
  fputws(L",\"message\":", stdout);
  write_json_string(stdout, message);
  if (windows_error) fwprintf(stdout, L",\"windowsError\":%lu", windows_error);
  fputws(L"}}\n", stdout);
}

void print_cli_validation_errors(bool json,
                                 const std::vector<std::wstring> &errors,
                                 const wchar_t *code) {
  if (! json) {
    for (const auto &error : errors) fwprintf(stderr, L"error: %ls\n", error.c_str());
    return;
  }
  fputws(L"{\"ok\":false,\"error\":{\"code\":", stdout);
  write_json_string(stdout, code ? code : L"validation");
  fputws(L",\"messages\":[", stdout);
  for (size_t index = 0; index < errors.size(); ++index) {
    if (index) fputwc(L',', stdout);
    write_json_string(stdout, errors[index]);
  }
  fputws(L"]}}\n", stdout);
}

void print_cli_success(bool json, const wchar_t *operation,
                       const std::wstring &service,
                       const std::wstring &detail) {
  if (! json) {
    if (! detail.empty()) fwprintf(stdout, L"%ls\n", detail.c_str());
    return;
  }
  fputws(L"{\"ok\":true,\"operation\":", stdout);
  write_json_string(stdout, operation ? operation : L"");
  fputws(L",\"service\":", stdout);
  write_json_string(stdout, service);
  if (! detail.empty()) {
    fputws(L",\"detail\":", stdout);
    write_json_string(stdout, detail);
  }
  fputws(L"}\n", stdout);
}

void print_cli_plan(bool json, const std::wstring &service,
                    const wchar_t *action,
                    const std::vector<std::wstring> &changes) {
  if (! json) {
    fwprintf(stdout, L"Plan for %ls: %ls\n", service.c_str(), action);
    for (const auto &change : changes) fwprintf(stdout, L"  - %ls\n", change.c_str());
    return;
  }
  fputws(L"{\"ok\":true,\"operation\":\"plan\",\"service\":", stdout);
  write_json_string(stdout, service);
  fputws(L",\"action\":", stdout);
  write_json_string(stdout, action ? action : L"");
  fputws(L",\"changes\":[", stdout);
  for (size_t index = 0; index < changes.size(); ++index) {
    if (index) fputwc(L',', stdout);
    write_json_string(stdout, changes[index]);
  }
  fputws(L"]}\n", stdout);
}
