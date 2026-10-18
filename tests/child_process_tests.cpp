#include "../child_process.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const wchar_t *message) {
  if (condition) return;
  std::wcerr << L"FAILED: " << message << L'\n';
  ++failures;
}

std::vector<wchar_t> make_block(
    std::initializer_list<const wchar_t *> values) {
  std::vector<wchar_t> block;
  for (const wchar_t *value : values) {
    while (*value) block.push_back(*value++);
    block.push_back(L'\0');
  }
  if (values.size() == 0) block.push_back(L'\0');
  block.push_back(L'\0');
  return block;
}

std::vector<environment_entry_t> parse(const std::vector<wchar_t> &block) {
  std::vector<environment_entry_t> entries;
  std::wstring error;
  expect(parse_environment_block(block.data(), &entries, &error),
         L"generated environment must be parseable");
  return entries;
}

void command_line_tests() {
  std::wstring command;
  std::wstring error;
  expect(build_child_command_line(LR"(C:\Program Files\App\app.exe)",
                                  L"--flag value", &command, &error),
         L"valid child command line should build");
  expect(command == LR"("C:\Program Files\App\app.exe" --flag value)",
         L"executable path should be quoted exactly once");
  expect(! build_child_command_line(L"", L"", &command, &error),
         L"empty executable should fail validation");
  expect(! build_child_command_line(LR"(C:\bad"path\app.exe)", L"", &command,
                                    &error),
         L"quote in executable should fail validation");
}

void environment_merge_tests() {
  auto base = make_block({L"Path=C:\\Windows", L"TEMP=C:\\Temp", L"=C:=C:\\"});
  auto extra = make_block({L"PATH=D:\\Tools", L"EMPTY="});
  std::vector<wchar_t> result;
  std::wstring error;
  expect(build_child_environment(base.data(), nullptr, extra.data(), &result,
                                 &error),
         L"environment merge should succeed");

  auto entries = parse(result);
  expect(entries.size() == 4, L"environment merge should preserve four keys");
  expect(entries[0].first == L"=C:" && entries[0].second == L"C:\\",
         L"drive current-directory entry should be preserved");
  expect(entries[2].first == L"Path" && entries[2].second == L"D:\\Tools",
         L"environment names should merge case-insensitively");
}

void environment_replacement_tests() {
  auto base = make_block({L"BASE=1"});
  auto replacement = make_block({L"ONLY=2"});
  auto extra = make_block({L"ADDED=3"});
  std::vector<wchar_t> result;
  std::wstring error;
  expect(build_child_environment(base.data(), replacement.data(), extra.data(),
                                 &result, &error),
         L"replacement environment should build");
  auto entries = parse(result);
  expect(entries.size() == 2, L"replacement must discard the base environment");
  expect(entries[0].first == L"ADDED" && entries[1].first == L"ONLY",
         L"environment block should be sorted case-insensitively");
}

void environment_validation_tests() {
  auto invalid = make_block({L"MISSING_SEPARATOR"});
  std::vector<wchar_t> result;
  std::wstring error;
  expect(! build_child_environment(invalid.data(), nullptr, nullptr, &result,
                                   &error),
         L"invalid environment entry should be rejected");

  auto empty = make_block({});
  expect(build_child_environment(empty.data(), nullptr, nullptr, &result, &error),
         L"empty environment should build");
  expect(result.size() == 2 && result[0] == L'\0' && result[1] == L'\0',
         L"empty environment must contain a double NUL terminator");
}

}  // namespace

int wmain() {
  command_line_tests();
  environment_merge_tests();
  environment_replacement_tests();
  environment_validation_tests();

  if (failures) {
    std::wcerr << failures << L" test assertion(s) failed\n";
    return 1;
  }
  std::wcout << L"All child process tests passed\n";
  return 0;
}
