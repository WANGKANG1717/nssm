#include <winsock2.h>
#include <ws2tcpip.h>

#include "child_process.h"
#include "command_runner.h"
#include "config.h"
#include "config_security.h"
#include "health_check.h"
#include "job_object.h"
#include "output.h"
#include "readiness.h"
#include "recovery.h"
#include "security.h"
#include "string_utils.h"
#include "utf8.h"
#include "win32_handle.h"

#include <aclapi.h>

#include <iostream>
#include <string>
#include <thread>
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

void cli_argument_tests() {
  wchar_t output_json[] = L"--output=json";
  wchar_t all[] = L"--all";
  wchar_t separator[] = L"--";
  wchar_t dash_name[] = L"-worker";
  wchar_t *valid[] = {output_json, all, separator, dash_name};
  cli_arguments_t parsed = parse_cli_arguments(4, valid);
  expect(parsed.json && parsed.all && ! parsed.invalid &&
             parsed.positional.size() == 1 &&
             std::wstring(parsed.positional.front()) == L"-worker",
         L"CLI parser should support systemctl-style options and -- separator");

  wchar_t unknown[] = L"--unknown";
  wchar_t *invalid[] = {unknown};
  parsed = parse_cli_arguments(1, invalid);
  expect(parsed.invalid && parsed.invalid_option == L"--unknown",
         L"unknown options must be rejected instead of becoming positional values");

  wchar_t legacy[] = L"--json";
  wchar_t *legacy_args[] = {legacy};
  parsed = parse_cli_arguments(1, legacy_args);
  expect(parsed.invalid,
         L"the old --json spelling should not remain as a hidden dialect");
}

void log_encoding_tests() {
  const unsigned char utf8[] = {'x', '\n'};
  expect(! encoded_newline_at(utf8, 0, sizeof(utf8), sizeof(char)) &&
             encoded_newline_at(utf8, 1, sizeof(utf8), sizeof(char)),
         L"UTF-8 newlines should be found on byte boundaries");

  const unsigned char utf16[] = {'x', 0, '\n', 0};
  expect(encoded_newline_at(utf16, 2, sizeof(utf16), sizeof(wchar_t)) &&
             ! encoded_newline_at(utf16, 3, sizeof(utf16), sizeof(wchar_t)) &&
             ! encoded_newline_at(utf16, 2, 3, sizeof(wchar_t)),
         L"UTF-16 newlines should include the complete code unit");
}

void string_utils_tests() {
  expect(strings_equal_ignore_case(L"ServiceName", L"servicename"),
         L"ordinal service name comparison should ignore ASCII case");
  expect(strings_equal_ignore_case(L"服务Ä", L"服务ä"),
         L"ordinal service name comparison should support Unicode case");
  expect(! strings_equal_ignore_case(L"service-a", L"service-b"),
         L"different service names must not compare equal");
  expect(strings_equal_ignore_case(nullptr, nullptr),
         L"two null strings should compare equal");
  expect(! strings_equal_ignore_case(nullptr, L"service"),
         L"a null and non-null string must not compare equal");
  expect(string_starts_with_ignore_case(L"NT service\\Example",
                                        L"NT SERVICE\\"),
         L"ordinal prefix comparison should ignore case");
  expect(! string_starts_with_ignore_case(L"NT SERVICE", L"NT SERVICE\\"),
         L"ordinal prefix comparison should require the complete prefix");
  ordinal_ignore_case_less less;
  expect(! less(L"Path", L"PATH") && ! less(L"PATH", L"Path"),
         L"ordinal comparator should treat environment names as equivalent");
  expect(less(L"alpha", L"beta") && ! less(L"beta", L"alpha"),
         L"ordinal comparator should provide stable ordering");
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
  constexpr const wchar_t *name = L"SVCPILOT_UNIT_ENVIRONMENT_ISOLATION";
  SetEnvironmentVariableW(name, L"wrapper-value");
  auto extra = make_block({L"SVCPILOT_UNIT_ENVIRONMENT_ISOLATION=child-value"});
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
  expect(configure_service_job_limits(job, 3, 64ULL * 1024ULL * 1024ULL,
                                      &error),
         L"Job Object resource limits should configure");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  expect(QueryInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &limits, sizeof(limits), nullptr) &&
             (limits.BasicLimitInformation.LimitFlags &
              JOB_OBJECT_LIMIT_ACTIVE_PROCESS) &&
             (limits.BasicLimitInformation.LimitFlags &
              JOB_OBJECT_LIMIT_JOB_MEMORY) &&
             limits.BasicLimitInformation.ActiveProcessLimit == 3 &&
             limits.JobMemoryLimit == 64ULL * 1024ULL * 1024ULL,
         L"configured Job Object limits should be queryable");

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
  expect(accounting.job_memory <= accounting.peak_job_memory,
         L"current Job memory should not exceed its recorded peak");
  job_accounting_t previous = {};
  job_accounting_t current = {};
  current.user_time_100ns = 500ULL * 10000ULL;
  expect(calculate_job_cpu_percent(previous, current, 1000, 1) == 50 &&
             calculate_job_cpu_percent(previous, current, 1000, 2) == 25,
         L"Job CPU usage should be normalized across active processors");

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
  constexpr const char *valid_toml = R"toml(
# Comments are part of the supported configuration format.
Schema = 1
[Unit]
Name = "示例服务"
DisplayName = "示例服务"
Requires = ["Tcpip"]
[Service]
ExecStart = 'C:\Apps\worker.exe'
Arguments = ["--name", "值 with spaces"]
Environment = ["MODE=production"]
Restart = "on-failure"
RestartSec = "1500ms"
RestartMaxDelaySec = "1min"
TimeoutStartSec = "45s"
TimeoutStopSec = "20s"
ExecStartPre = [
  { Path = 'C:\Windows\System32\cmd.exe', Arguments = ["/d", "/c", "exit 0"] },
  { Path = 'C:\Windows\System32\where.exe', Arguments = ["cmd.exe"] }
]
ExecStopPost = [{ Path = 'C:\Windows\System32\cmd.exe' }]
MemoryMax = 268435456
TasksMax = 8
CPUQuota = 75
[Windows]
Account = "system"
ServiceSid = "restricted"
)toml";
  service_config_t config;
  config_validation_t validation;
  expect(parse_service_config(valid_toml, strlen(valid_toml), &config,
                              &validation),
         L"valid declarative configuration should parse");
  expect(config.schema == 1 && config.service.name == L"示例服务",
         L"UTF-8 service metadata should convert to UTF-16");
  expect(config.application.arguments.size() == 2 &&
             config.application.environment.size() == 1,
         L"application arrays and environment should be retained");
  expect(config.recovery.initial_delay_ms == 1500 &&
             config.recovery.maximum_delay_ms == 60000,
         L"systemd-style duration values should convert to milliseconds");
  expect(config.resources.memory_limit_bytes == 268435456ULL &&
             config.resources.maximum_processes == 8 &&
             config.resources.maximum_cpu_percent == 75,
         L"systemd-style resource limits should map to the runtime model");
  expect(config.lifecycle.exec_start_pre.size() == 2 &&
             config.lifecycle.exec_stop_post.size() == 1 &&
             config.lifecycle.timeout_start_ms == 45000 &&
             config.lifecycle.timeout_stop_ms == 20000,
         L"lifecycle hooks and timeouts should parse into the runtime model");

  constexpr const char *malformed_toml = "schema = [";
  expect(! parse_service_config(malformed_toml, strlen(malformed_toml),
                                &config, &validation) &&
             ! validation.errors.empty() &&
             validation.errors.front().find(L"line 1") != std::wstring::npos,
         L"TOML syntax errors should include their source line");

  constexpr const char *legacy_json = R"json({"schema": 1})json";
  expect(! parse_service_config(legacy_json, strlen(legacy_json), &config,
                                &validation),
         L"legacy JSON configuration should not be accepted implicitly");

  constexpr const char *legacy_schema = R"toml(
schema = 1
[service]
name = "worker"
[application]
path = 'C:\Apps\worker.exe'
)toml";
  expect(! parse_service_config(legacy_schema, strlen(legacy_schema), &config,
                                &validation),
         L"legacy camelCase configuration should not remain as a hidden dialect");

  constexpr const char *embedded_nul_toml = R"toml(
Schema = 1
[Unit]
Name = "worker\u0000hidden"
[Service]
ExecStart = 'C:\Apps\worker.exe'
)toml";
  expect(! parse_service_config(embedded_nul_toml, strlen(embedded_nul_toml),
                                &config, &validation),
         L"configuration strings containing U+0000 must be rejected");

  constexpr const char *invalid_toml = R"toml(
Schema = 1
Unknown = true
[Unit]
Name = "bad/name"
[Service]
ExecStart = "relative.exe"
Environment = ["Path=one", "PATH=two"]
)toml";
  expect(! parse_service_config(invalid_toml, strlen(invalid_toml), &config,
                               &validation),
         L"unsafe or ambiguous configuration should be rejected");
  expect(validation.errors.size() >= 4,
         L"configuration validation should report all independent errors");

  constexpr const char *invalid_hook = R"toml(
Schema = 1
[Unit]
Name = "bad-hook"
[Service]
ExecStart = 'C:\Apps\worker.exe'
ExecStartPre = [{ Path = "relative.exe" }]
TimeoutStopSec = "10ms"
)toml";
  expect(! parse_service_config(invalid_hook, strlen(invalid_hook), &config,
                                &validation),
         L"relative lifecycle commands and undersized timeouts must be rejected");

  constexpr const char *password_toml = R"toml(
Schema = 1
[Unit]
Name = "secure"
[Service]
ExecStart = 'C:\Apps\worker.exe'
[Windows]
Account = "user"
AccountName = 'DOMAIN\svc'
Password = "bad"
)toml";
  expect(! parse_service_config(password_toml, strlen(password_toml), &config,
                                &validation),
         L"plaintext password fields must not be accepted");
}

void command_runner_tests() {
  wchar_t system_directory[MAX_PATH] = {};
  const UINT length = GetSystemDirectoryW(
      system_directory, static_cast<UINT>(std::size(system_directory)));
  expect(length && length < std::size(system_directory),
         L"system directory should be available to lifecycle tests");
  if (! length || length >= std::size(system_directory)) return;

  command_config_t command;
  command.path = std::wstring(system_directory) + L"\\cmd.exe";
  command.arguments = {L"/d", L"/c", L"exit 0"};
  command_result_t result = run_command(command, nullptr, system_directory,
                                        nullptr, 5000);
  expect(result.succeeded() && result.exit_code == 0,
         L"lifecycle command runner should report successful commands");

  command.arguments = {L"/d", L"/c", L"exit 7"};
  result = run_command(command, nullptr, system_directory, nullptr, 5000);
  expect(result.state == command_state_t::failed && result.exit_code == 7,
         L"lifecycle command runner should preserve a non-zero exit code");

  command.path = std::wstring(system_directory) + L"\\ping.exe";
  command.arguments = {L"-n", L"30", L"127.0.0.1"};
  result = run_command(command, nullptr, system_directory, nullptr, 10);
  expect(result.state == command_state_t::timed_out,
         L"lifecycle command runner should enforce its deadline");

  unique_handle cancelled(CreateEventW(nullptr, TRUE, TRUE, nullptr));
  result = run_command(command, nullptr, system_directory, cancelled.get(),
                       5000);
  expect(result.state == command_state_t::cancelled,
         L"lifecycle command runner should respond to cancellation");
}

void recovery_tests() {
  recovery_config_t config;
  config.jitter_percent = 0;
  config.initial_delay_ms = 1000;
  config.maximum_delay_ms = 5000;
  config.multiplier = 2;
  config.maximum_restarts = 3;
  config.window_ms = 10000;
  config.reset_after_ms = 6000;
  recovery_state_t state;

  auto decision = evaluate_recovery(config, &state, 1, 1000, 100, 0);
  expect(decision.should_restart() && decision.delay_ms == 1000,
         L"first failed run should use the initial recovery delay");
  decision = evaluate_recovery(config, &state, 1, 2000, 100, 0);
  expect(decision.should_restart() && decision.delay_ms == 2000,
         L"recovery delay should grow exponentially");
  decision = evaluate_recovery(config, &state, 1, 3000, 100, 0);
  expect(decision.should_restart() && decision.delay_ms == 4000,
         L"third recovery delay should continue exponential growth");
  decision = evaluate_recovery(config, &state, 1, 4000, 100, 0);
  expect(decision.result == recovery_result_t::rate_limited,
         L"restart storms should stop at the configured window limit");

  decision = evaluate_recovery(config, &state, 1, 20000, 7000, 0);
  expect(decision.should_restart() && decision.delay_ms == 1000,
         L"a stable run should reset backoff and restart history");

  config.mode = recovery_mode_t::on_failure;
  decision = evaluate_recovery(config, &state, 0, 21000, 100, 0);
  expect(decision.result == recovery_result_t::clean_exit,
         L"onFailure recovery should not restart a clean exit");
  config.mode = recovery_mode_t::never;
  decision = evaluate_recovery(config, &state, 1, 22000, 100, 0);
  expect(decision.result == recovery_result_t::mode_disabled,
         L"never recovery should suppress all restarts");

  config.mode = recovery_mode_t::always;
  config.jitter_percent = 100;
  state.consecutive_restarts = 20;
  state.restart_times_ms.clear();
  decision = evaluate_recovery(config, &state, 0, 30000, 100, 4999);
  expect(decision.should_restart() &&
             decision.delay_ms <= config.maximum_delay_ms,
         L"jitter must never exceed the configured maximum delay");
}

SOCKET create_loopback_listener(unsigned short *port) {
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  expect(listener != INVALID_SOCKET, L"loopback listener socket should open");
  if (listener == INVALID_SOCKET) return listener;
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
      listen(listener, 1)) {
    expect(false, L"loopback listener should bind and listen");
    closesocket(listener);
    return INVALID_SOCKET;
  }
  int length = sizeof(address);
  if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length)) {
    expect(false, L"loopback listener should expose its port");
    closesocket(listener);
    return INVALID_SOCKET;
  }
  *port = ntohs(address.sin_port);
  return listener;
}

void health_probe_tests() {
  health_check_config_t config;
  config.type = health_check_type_t::process;
  auto result = run_health_probe(config, GetCurrentProcess(), nullptr, nullptr);
  expect(result.healthy, L"current process health probe should succeed");

  wchar_t current_directory[MAX_PATH] = {};
  unsigned long current_length = GetCurrentDirectoryW(
      static_cast<unsigned long>(std::size(current_directory)),
      current_directory);
  expect(current_length > 0 && current_length < std::size(current_directory),
         L"test working directory should be available");
  std::wstring heartbeat = std::wstring(current_directory) +
      L"\\svcpilot-health-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  HANDLE file = CreateFileW(heartbeat.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  expect(file != INVALID_HANDLE_VALUE,
         L"heartbeat fixture should be created inside the build directory");
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  config = {};
  config.type = health_check_type_t::file;
  config.file_path = heartbeat;
  config.maximum_age_ms = 60000;
  result = run_health_probe(config, nullptr, nullptr, nullptr);
  expect(result.healthy, L"fresh heartbeat file should be healthy");

  file = CreateFileW(heartbeat.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    FILETIME now_file = {};
    GetSystemTimeAsFileTime(&now_file);
    ULARGE_INTEGER old = {};
    old.LowPart = now_file.dwLowDateTime;
    old.HighPart = now_file.dwHighDateTime;
    old.QuadPart -= 120000ULL * 10000ULL;
    FILETIME old_file = {old.LowPart, old.HighPart};
    SetFileTime(file, nullptr, nullptr, &old_file);
    CloseHandle(file);
  }
  result = run_health_probe(config, nullptr, nullptr, nullptr);
  expect(! result.healthy, L"stale heartbeat file should fail");
  DeleteFileW(heartbeat.c_str());

  wchar_t system_directory[MAX_PATH] = {};
  UINT system_length = GetSystemDirectoryW(
      system_directory, static_cast<UINT>(std::size(system_directory)));
  expect(system_length > 0 && system_length < std::size(system_directory),
         L"system directory should be available for command probe");
  config = {};
  config.type = health_check_type_t::command;
  config.timeout_ms = 5000;
  config.command_path = std::wstring(system_directory) + L"\\cmd.exe";
  config.command_arguments = {L"/d", L"/c", L"exit /b 0"};
  const wchar_t empty_environment[] = {L'\0', L'\0'};
  result = run_health_probe(config, nullptr, empty_environment,
                            system_directory);
  expect(result.healthy, L"zero-exit command probe should succeed");

  WSADATA winsock = {};
  expect(! WSAStartup(MAKEWORD(2, 2), &winsock),
         L"Winsock should start for local probe fixtures");
  unsigned short port = 0;
  SOCKET listener = create_loopback_listener(&port);
  if (listener != INVALID_SOCKET) {
    config = {};
    config.type = health_check_type_t::tcp;
    config.host = L"127.0.0.1";
    config.port = port;
    config.timeout_ms = 2000;
    result = run_health_probe(config, nullptr, nullptr, nullptr);
    expect(result.healthy, L"TCP probe should connect to a loopback listener");
    closesocket(listener);
  }

  listener = create_loopback_listener(&port);
  if (listener != INVALID_SOCKET) {
    std::thread server([listener]() {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(listener, &readable);
      timeval timeout = {5, 0};
      if (select(0, &readable, nullptr, nullptr, &timeout) > 0) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client != INVALID_SOCKET) {
          unsigned long receive_timeout = 5000;
          setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&receive_timeout),
                     sizeof(receive_timeout));
          char request[1024];
          recv(client, request, sizeof(request), 0);
          constexpr char response[] =
              "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
          send(client, response, static_cast<int>(sizeof(response) - 1), 0);
          closesocket(client);
        }
      }
      closesocket(listener);
    });
    config = {};
    config.type = health_check_type_t::http;
    config.url = L"http://127.0.0.1:" + std::to_wstring(port) + L"/health";
    config.timeout_ms = 2000;
    result = run_health_probe(config, nullptr, nullptr, nullptr);
    expect(result.healthy && result.detail == L"HTTP status 204",
           L"HTTP probe should validate a local 204 response");
    server.join();
  }
  WSACleanup();
}

void readiness_tests() {
  readiness_config_t config;
  config.probe.type = health_check_type_t::process;
  config.probe.interval_ms = 100;
  config.probe.success_threshold = 2;
  config.startup_timeout_ms = 1000;
  readiness_result_t result = wait_for_readiness(
      config, GetCurrentProcess(), nullptr, nullptr);
  expect(result.ready(),
         L"readiness should require the configured consecutive successes");

  config = {};
  config.probe.type = health_check_type_t::file;
  config.probe.file_path = L"svcpilot-readiness-missing-" +
      std::to_wstring(GetCurrentProcessId());
  config.probe.interval_ms = 100;
  config.startup_timeout_ms = 250;
  result = wait_for_readiness(config, GetCurrentProcess(), nullptr, nullptr);
  expect(result.state == readiness_state_t::timed_out &&
             result.error == ERROR_TIMEOUT,
         L"readiness should fail when its startup deadline expires");

  unique_handle cancellation(CreateEventW(nullptr, TRUE, TRUE, nullptr));
  config = {};
  config.probe.type = health_check_type_t::process;
  config.startup_timeout_ms = 1000;
  result = wait_for_readiness(config, GetCurrentProcess(), nullptr, nullptr,
                              cancellation.get());
  expect(result.state == readiness_state_t::cancelled &&
             result.error == ERROR_CANCELLED,
         L"readiness should stop when service cancellation is signalled");
}

void path_security_tests() {
  wchar_t system_directory[MAX_PATH] = {};
  expect(GetSystemDirectoryW(system_directory, _countof(system_directory)) != 0,
         L"the Windows system directory should be available");
  std::wstring cmd_path = std::wstring(system_directory) + L"\\cmd.exe";
  config_validation_t validation;
  expect(audit_secure_path(cmd_path, L"test.systemBinary", true, &validation),
         L"a protected Windows system binary should pass ACL auditing");
  unique_handle cmd_file(CreateFileW(
      cmd_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr));
  std::wstring handle_reason;
  expect(cmd_file && check_secure_file_handle(cmd_file.get(), &handle_reason),
         L"a handle to a protected Windows system binary should pass ACL auditing");

  wchar_t temporary_directory[MAX_PATH] = {};
  wchar_t temporary_file[MAX_PATH] = {};
  if (! GetTempPathW(_countof(temporary_directory), temporary_directory) ||
      ! GetTempFileNameW(temporary_directory, L"nsm", 0, temporary_file)) {
    expect(false, L"an ACL test file should be created");
    return;
  }
  std::vector<unsigned char> everyone(SECURITY_MAX_SID_SIZE);
  unsigned long sid_size = static_cast<unsigned long>(everyone.size());
  EXPLICIT_ACCESSW access = {};
  PACL acl = nullptr;
  if (CreateWellKnownSid(WinWorldSid, nullptr, everyone.data(), &sid_size)) {
    access.grfAccessPermissions = GENERIC_WRITE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    BuildTrusteeWithSidW(&access.Trustee, everyone.data());
    if (SetEntriesInAclW(1, &access, nullptr, &acl) == ERROR_SUCCESS) {
      SetNamedSecurityInfoW(temporary_file, SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION, nullptr, nullptr, acl,
                            nullptr);
    }
  }
  validation.errors.clear();
  expect(! audit_secure_path(temporary_file, L"test.insecure", true,
                             &validation),
         L"a file writable by Everyone must fail ACL auditing");
  unique_handle insecure_file(CreateFileW(
      temporary_file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr));
  handle_reason.clear();
  expect(insecure_file &&
             ! check_secure_file_handle(insecure_file.get(), &handle_reason),
         L"a handle to a file writable by Everyone must fail ACL auditing");
  if (acl) LocalFree(acl);
  DeleteFileW(temporary_file);
}

void debug_privilege_guard_tests() {
  unique_handle before;
  const bool had_thread_token =
      OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE, before.put()) !=
      FALSE;
  const unsigned long before_error =
      had_thread_token ? ERROR_SUCCESS : GetLastError();
  before.reset();

  {
    debug_privilege_guard privilege;
    if (privilege) {
      unique_handle token;
      expect(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE,
                             token.put()) != FALSE,
             L"an enabled debug privilege must belong to the current thread token");
      LUID luid = {};
      PRIVILEGE_SET requested = {};
      requested.PrivilegeCount = 1;
      requested.Control = PRIVILEGE_SET_ALL_NECESSARY;
      requested.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;
      BOOL enabled = FALSE;
      const bool checked =
          token && LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid);
      requested.Privilege[0].Luid = luid;
      expect(checked && PrivilegeCheck(token.get(), &requested, &enabled) &&
                 enabled,
             L"debug privilege guard must enable SeDebugPrivilege in scope");
    }
  }

  unique_handle after;
  const bool has_thread_token =
      OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE, after.put()) !=
      FALSE;
  const unsigned long after_error =
      has_thread_token ? ERROR_SUCCESS : GetLastError();
  expect(had_thread_token == has_thread_token &&
             (had_thread_token ||
              (before_error == ERROR_NO_TOKEN && after_error == ERROR_NO_TOKEN)),
         L"debug privilege guard must restore the original thread-token state");
}

}  // namespace

int wmain() {
  command_line_tests();
  cli_argument_tests();
  log_encoding_tests();
  string_utils_tests();
  environment_merge_tests();
  environment_replacement_tests();
  environment_validation_tests();
  environment_isolation_tests();
  job_object_tests();
  configuration_tests();
  command_runner_tests();
  recovery_tests();
  health_probe_tests();
  readiness_tests();
  path_security_tests();
  debug_privilege_guard_tests();

  if (failures) {
    std::wcerr << failures << L" test assertion(s) failed\n";
    return 1;
  }
  std::wcout << L"All child process tests passed\n";
  return 0;
}
