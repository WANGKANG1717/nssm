#include "../child_process.h"
#include "../config.h"
#include "../job_object.h"

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
  expect(! build_child_command_line(L"app.exe", std::wstring(32767, L'x'),
                                    &command, &error),
         L"oversized command line should fail validation");

  std::vector<std::wstring> arguments = {
      L"plain", L"two words", L"", L"quoted\"value", L"C:\\path with space\\"};
  expect(build_child_command_line(LR"(C:\Apps\app.exe)", arguments, &command,
                                  &error),
         L"argument array should build a command line");
  expect(command ==
             LR"("C:\Apps\app.exe" plain "two words" "" "quoted\"value" "C:\path with space\\")",
         L"argument array should follow Windows backslash and quote rules");
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

void environment_isolation_tests() {
  constexpr const wchar_t *name = L"NSSM_UNIT_ENVIRONMENT_ISOLATION";
  SetEnvironmentVariableW(name, L"wrapper-value");
  auto extra = make_block({L"NSSM_UNIT_ENVIRONMENT_ISOLATION=child-value"});
  std::vector<wchar_t> result;
  std::wstring error;
  expect(build_child_environment(nullptr, nullptr, extra.data(), &result, &error),
         L"isolated child environment should build");

  wchar_t value[64] = {};
  GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
  expect(std::wstring(value) == L"wrapper-value",
         L"building a child environment must not mutate the wrapper");
  SetEnvironmentVariableW(name, nullptr);
}

void job_object_tests() {
  unsigned long error = ERROR_SUCCESS;
  HANDLE job = create_service_job(&error);
  expect(job != nullptr && error == ERROR_SUCCESS,
         L"kill-on-close job should be created");
  if (! job) return;

  wchar_t system_directory[MAX_PATH] = {};
  UINT length = GetSystemDirectoryW(system_directory,
                                    static_cast<UINT>(std::size(system_directory)));
  expect(length > 0 && length < std::size(system_directory),
         L"system directory should be available");
  std::wstring executable = std::wstring(system_directory) + L"\\cmd.exe";
  std::wstring command = L"\"" + executable + L"\" /d /c exit 0";

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  bool created = CreateProcessW(executable.c_str(), command.data(), nullptr,
                                nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                                nullptr, &startup, &process) != FALSE;
  expect(created, L"job test child should start suspended");
  if (! created) {
    CloseHandle(job);
    return;
  }

  expect(assign_process_to_service_job(job, process.hProcess, &error),
         L"child process should join the job");
  ResumeThread(process.hThread);
  expect(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
         L"job test child should exit");

  job_accounting_t accounting = {};
  expect(query_service_job(job, &accounting, &error),
         L"job accounting should be queryable");
  expect(accounting.active_processes == 0 && accounting.total_processes >= 1,
         L"job accounting should record the completed child");

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(job);

  job = create_service_job(&error);
  expect(job != nullptr, L"second kill-on-close job should be created");
  if (! job) return;

  executable = std::wstring(system_directory) + L"\\ping.exe";
  command = L"\"" + executable + L"\" -n 60 127.0.0.1";
  process = {};
  created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                           FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &process) != FALSE;
  expect(created, L"long-running job test child should start suspended");
  if (! created) {
    CloseHandle(job);
    return;
  }
  expect(assign_process_to_service_job(job, process.hProcess, &error),
         L"long-running child should join the job");
  ResumeThread(process.hThread);
  CloseHandle(job);
  expect(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0,
         L"closing the job must terminate its child process tree");
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

void configuration_tests() {
  constexpr const char *valid_json = R"json({
    "schema": 1,
    "service": {
      "name": "示例服务",
      "displayName": "示例服务",
      "startup": "delayed",
      "account": { "type": "system" },
      "dependencies": ["Tcpip"]
    },
    "application": {
      "path": "C:\\Apps\\worker.exe",
      "arguments": ["--name", "值 with spaces"],
      "inheritEnvironment": true,
      "environment": { "MODE": "production" }
    }
  })json";
  service_config_t config;
  config_validation_t validation;
  expect(parse_service_config(valid_json, strlen(valid_json), &config,
                              &validation),
         L"valid declarative configuration should parse");
  expect(config.schema == 1 && config.service.name == L"示例服务",
         L"UTF-8 service metadata should convert to UTF-16");
  expect(config.application.arguments.size() == 2 &&
             config.application.environment.size() == 1,
         L"application arrays and environment should be retained");

  constexpr const char *invalid_json = R"json({
    "schema": 1,
    "unknown": true,
    "service": { "name": "bad/name" },
    "application": {
      "path": "relative.exe",
      "environment": { "Path": "one", "PATH": "two" }
    }
  })json";
  expect(! parse_service_config(invalid_json, strlen(invalid_json), &config,
                               &validation),
         L"unsafe or ambiguous configuration should be rejected");
  expect(validation.errors.size() >= 4,
         L"configuration validation should report all independent errors");

  constexpr const char *password_json = R"json({
    "schema": 1,
    "service": {
      "name": "secure",
      "account": { "type": "user", "name": "DOMAIN\\svc", "password": "bad" }
    },
    "application": { "path": "C:\\Apps\\worker.exe" }
  })json";
  expect(! parse_service_config(password_json, strlen(password_json), &config,
                               &validation),
         L"plaintext password fields must not be accepted");
}

}  // namespace

int wmain() {
  command_line_tests();
  environment_merge_tests();
  environment_replacement_tests();
  environment_validation_tests();
  environment_isolation_tests();
  job_object_tests();
  configuration_tests();

  if (failures) {
    std::wcerr << failures << L" test assertion(s) failed\n";
    return 1;
  }
  std::wcout << L"All child process tests passed\n";
  return 0;
}
