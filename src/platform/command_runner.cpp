#include "command_runner.h"

#include "child_process.h"
#include "job_object.h"
#include "win32_handle.h"

#include <vector>

namespace {

command_result_t failure(command_state_t state, unsigned long error,
                         std::wstring detail) {
  command_result_t result;
  result.state = state;
  result.windows_error = error;
  result.detail = std::move(detail);
  return result;
}

}  // namespace

command_result_t run_command(const command_config_t &command,
                             const wchar_t *environment,
                             const wchar_t *working_directory,
                             HANDLE cancellation_event,
                             unsigned long timeout_ms) {
  std::wstring command_line;
  std::wstring error_text;
  if (! build_child_command_line(command.path, command.arguments,
                                 &command_line, &error_text)) {
    return failure(command_state_t::failed, ERROR_INVALID_PARAMETER,
                   std::move(error_text));
  }

  unsigned long error = ERROR_SUCCESS;
  unique_handle job(create_service_job(&error));
  if (! job) {
    return failure(command_state_t::failed, error,
                   L"creating command Job Object failed");
  }

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (! CreateProcessW(
          command.path.c_str(), command_line.data(), nullptr, nullptr, FALSE,
          CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
          const_cast<wchar_t *>(environment), working_directory, &startup,
          &process)) {
    return failure(command_state_t::failed, GetLastError(),
                   L"starting command failed");
  }
  unique_handle thread(process.hThread);
  unique_handle process_handle(process.hProcess);
  if (! assign_process_to_service_job(job.get(), process_handle.get(), &error)) {
    TerminateProcess(process_handle.get(), error);
    return failure(command_state_t::failed, error,
                   L"assigning command to Job Object failed");
  }
  if (ResumeThread(thread.get()) == static_cast<unsigned long>(-1)) {
    error = GetLastError();
    terminate_service_job(job.get(), error, nullptr);
    WaitForSingleObject(process_handle.get(), 5000);
    return failure(command_state_t::failed, error,
                   L"resuming command failed");
  }
  thread.reset();

  HANDLE handles[2] = {process_handle.get(), cancellation_event};
  const unsigned long count = cancellation_event ? 2 : 1;
  const unsigned long wait = WaitForMultipleObjects(count, handles, FALSE,
                                                     timeout_ms);
  if (wait != WAIT_OBJECT_0) {
    const bool cancelled = cancellation_event && wait == WAIT_OBJECT_0 + 1;
    error = cancelled ? ERROR_CANCELLED
                      : wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    terminate_service_job(job.get(), error, nullptr);
    WaitForSingleObject(process_handle.get(), 5000);
    return failure(cancelled ? command_state_t::cancelled
                             : wait == WAIT_TIMEOUT ? command_state_t::timed_out
                                                    : command_state_t::failed,
                   error, cancelled ? L"command was cancelled"
                                    : wait == WAIT_TIMEOUT
                                          ? L"command timed out"
                                          : L"waiting for command failed");
  }

  command_result_t result;
  if (! GetExitCodeProcess(process_handle.get(), &result.exit_code)) {
    return failure(command_state_t::failed, GetLastError(),
                   L"reading command exit code failed");
  }
  if (result.exit_code) {
    result.state = command_state_t::failed;
    result.windows_error = ERROR_PROCESS_ABORTED;
    result.detail = L"command exited with code " +
                    std::to_wstring(result.exit_code);
    return result;
  }
  result.state = command_state_t::succeeded;
  result.detail = L"command succeeded";
  return result;
}
