#include <winsock2.h>
#include <ws2tcpip.h>

#include "../child_process.h"
#include "../config.h"
#include "../health_check.h"
#include "../job_object.h"
#include "../readiness.h"
#include "../recovery.h"

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

  constexpr const char *virtual_account_json = R"json({
    "schema": 1,
    "service": {
      "name": "worker",
      "account": { "type": "virtual" }
    },
    "application": { "path": "C:\\Apps\\worker.exe" }
  })json";
  expect(parse_service_config(virtual_account_json,
                              strlen(virtual_account_json), &config,
                              &validation),
         L"a virtual account should derive its name from the service name");

  constexpr const char *mismatched_virtual_account_json = R"json({
    "schema": 1,
    "service": {
      "name": "worker",
      "account": { "type": "virtual", "name": "other" }
    },
    "application": { "path": "C:\\Apps\\worker.exe" }
  })json";
  expect(! parse_service_config(mismatched_virtual_account_json,
                                strlen(mismatched_virtual_account_json),
                                &config, &validation),
         L"an explicit virtual account name must match the service name");

  constexpr const char *invalid_gmsa_json = R"json({
    "schema": 1,
    "service": {
      "name": "worker",
      "account": { "type": "gmsa", "name": "DOMAIN\\worker" }
    },
    "application": { "path": "C:\\Apps\\worker.exe" }
  })json";
  expect(! parse_service_config(invalid_gmsa_json, strlen(invalid_gmsa_json),
                                &config, &validation),
         L"a gMSA account name must end with a dollar sign");

  constexpr const char *embedded_nul_json = R"json({
    "schema": 1,
    "service": { "name": "worker\u0000hidden" },
    "application": { "path": "C:\\Apps\\worker.exe" }
  })json";
  expect(! parse_service_config(embedded_nul_json, strlen(embedded_nul_json),
                                &config, &validation),
         L"configuration strings containing U+0000 must be rejected");

  constexpr const char *tcp_health_json = R"json({
    "schema": 1,
    "service": { "name": "worker" },
    "application": { "path": "C:\\Apps\\worker.exe" },
    "health": {
      "type": "tcp",
      "timeoutMs": 2000,
      "tcp": { "host": "127.0.0.1", "port": 8080 }
    }
  })json";
  expect(parse_service_config(tcp_health_json, strlen(tcp_health_json),
                              &config, &validation) &&
             config.health.type == health_check_type_t::tcp &&
             config.health.port == 8080,
         L"TCP health configuration should parse strictly");

  constexpr const char *mixed_health_json = R"json({
    "schema": 1,
    "service": { "name": "worker" },
    "application": { "path": "C:\\Apps\\worker.exe" },
    "health": {
      "type": "tcp",
      "tcp": { "host": "127.0.0.1", "port": 8080 },
      "file": { "path": "C:\\heartbeat", "maximumAgeMs": 1000 }
    }
  })json";
  expect(! parse_service_config(mixed_health_json, strlen(mixed_health_json),
                                &config, &validation),
         L"health settings for an unselected probe type must be rejected");

  constexpr const char *readiness_json = R"json({
    "schema": 1,
    "service": { "name": "worker" },
    "application": { "path": "C:\\Apps\\worker.exe" },
    "readiness": {
      "type": "tcp",
      "startupTimeoutMs": 30000,
      "successThreshold": 2,
      "tcp": { "host": "127.0.0.1", "port": 8080 }
    }
  })json";
  expect(parse_service_config(readiness_json, strlen(readiness_json),
                              &config, &validation) &&
             config.readiness.probe.type == health_check_type_t::tcp &&
             config.readiness.probe.success_threshold == 2 &&
             config.readiness.startup_timeout_ms == 30000,
         L"readiness configuration should parse independently from health");

  constexpr const char *resource_json = R"json({
    "schema": 1,
    "service": { "name": "worker" },
    "application": { "path": "C:\\Apps\\worker.exe" },
    "resources": {
      "monitorIntervalMs": 1000,
      "memoryLimitBytes": 268435456,
      "maximumProcesses": 8,
      "cpu": { "maximumPercent": 75, "violationThreshold": 3 }
    }
  })json";
  expect(parse_service_config(resource_json, strlen(resource_json), &config,
                              &validation) &&
             config.resources.memory_limit_bytes == 268435456ULL &&
             config.resources.maximum_processes == 8 &&
             config.resources.maximum_cpu_percent == 75,
         L"resource limits should parse from declarative configuration");

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
      L"\\nssm-health-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
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
  config.probe.file_path = L"nssm-readiness-missing-" +
      std::to_wstring(GetCurrentProcessId());
  config.probe.interval_ms = 100;
  config.startup_timeout_ms = 250;
  result = wait_for_readiness(config, GetCurrentProcess(), nullptr, nullptr);
  expect(result.state == readiness_state_t::timed_out &&
             result.error == ERROR_TIMEOUT,
         L"readiness should fail when its startup deadline expires");
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
  recovery_tests();
  health_probe_tests();
  readiness_tests();

  if (failures) {
    std::wcerr << failures << L" test assertion(s) failed\n";
    return 1;
  }
  std::wcout << L"All child process tests passed\n";
  return 0;
}
