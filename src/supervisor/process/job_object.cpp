#include "job_object.h"
#include "win32_handle.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace {

/*
 * JobObjectMemoryUsageInformation (28) and its public structure are available
 * on Windows 8 and later, but older MinGW headers still expose the information
 * class under its reserved name.
 */
constexpr JOBOBJECTINFOCLASS kJobObjectMemoryUsageInformation =
    static_cast<JOBOBJECTINFOCLASS>(28);

struct job_memory_usage_information_t {
  unsigned long long job_memory;
  unsigned long long peak_job_memory_used;
};

struct job_process_info_t {
  unsigned long process_id;
  unsigned long long creation_time;
  bool protected_process;
};

void save_error(unsigned long *error, unsigned long value) {
  if (error) *error = value;
}

unsigned long long file_time_value(const FILETIME &time) {
  ULARGE_INTEGER value = {};
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return value.QuadPart;
}

bool query_job_process_ids(HANDLE job, std::vector<unsigned long> *processes,
                           unsigned long *assigned_processes,
                           unsigned long *error) {
  size_t capacity = 32;
  for (unsigned long attempt = 0; attempt < 4; ++attempt) {
    const size_t bytes =
        offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) +
        capacity * sizeof(ULONG_PTR);
    std::vector<unsigned char> buffer(bytes);
    auto *list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(
        buffer.data());
    DWORD returned = 0;
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list,
                                  static_cast<DWORD>(buffer.size()),
                                  &returned)) {
      const size_t count = std::min<size_t>(
          list->NumberOfProcessIdsInList, capacity);
      processes->clear();
      processes->reserve(count);
      for (size_t index = 0; index < count; ++index) {
        processes->push_back(static_cast<unsigned long>(
            list->ProcessIdList[index]));
      }
      if (assigned_processes) {
        *assigned_processes = list->NumberOfAssignedProcesses;
      }
      save_error(error, ERROR_SUCCESS);
      return true;
    }
    const unsigned long query_error = GetLastError();
    if (query_error != ERROR_MORE_DATA) {
      save_error(error, query_error);
      return false;
    }
    capacity = std::max<size_t>(
        capacity * 2, static_cast<size_t>(list->NumberOfAssignedProcesses) + 8);
  }
  save_error(error, ERROR_MORE_DATA);
  return false;
}

}  // namespace

HANDLE create_service_job(unsigned long *error) {
  unique_handle job(CreateJobObjectW(nullptr, nullptr));
  if (! job) {
    save_error(error, GetLastError());
    return nullptr;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (! SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits))) {
    save_error(error, GetLastError());
    return nullptr;
  }

  save_error(error, ERROR_SUCCESS);
  return job.release();
}

bool configure_service_job_limits(HANDLE job,
                                  unsigned long maximum_processes,
                                  unsigned long long memory_limit_bytes,
                                  unsigned long *error) {
  if (! job || memory_limit_bytes > static_cast<unsigned long long>(SIZE_MAX)) {
    save_error(error, ERROR_INVALID_PARAMETER);
    return false;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  if (! QueryInformationJobObject(job, JobObjectExtendedLimitInformation,
                                  &limits, sizeof(limits), nullptr)) {
    save_error(error, GetLastError());
    return false;
  }
  if (maximum_processes) {
    limits.BasicLimitInformation.LimitFlags |=
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = maximum_processes;
  }
  if (memory_limit_bytes) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
    limits.JobMemoryLimit = static_cast<size_t>(memory_limit_bytes);
  }
  if (! SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits))) {
    save_error(error, GetLastError());
    return false;
  }
  save_error(error, ERROR_SUCCESS);
  return true;
}

bool assign_process_to_service_job(HANDLE job, HANDLE process,
                                   unsigned long *error) {
  if (! job || ! process) {
    save_error(error, ERROR_INVALID_HANDLE);
    return false;
  }
  if (! AssignProcessToJobObject(job, process)) {
    save_error(error, GetLastError());
    return false;
  }
  save_error(error, ERROR_SUCCESS);
  return true;
}

bool enforce_service_job_process_limit(HANDLE job,
                                       unsigned long maximum_processes,
                                       unsigned long protected_process_id,
                                       unsigned long *error) {
  if (! job) {
    save_error(error, ERROR_INVALID_HANDLE);
    return false;
  }
  if (! maximum_processes) {
    save_error(error, ERROR_SUCCESS);
    return true;
  }

  std::vector<unsigned long> process_ids;
  unsigned long assigned_processes = 0;
  if (! query_job_process_ids(job, &process_ids, &assigned_processes, error)) {
    return false;
  }
  if (process_ids.size() <= maximum_processes) {
    save_error(error, ERROR_SUCCESS);
    return true;
  }

  std::vector<job_process_info_t> processes;
  processes.reserve(process_ids.size());
  const unsigned long current_process_id = GetCurrentProcessId();
  for (unsigned long process_id : process_ids) {
    job_process_info_t info = {};
    info.process_id = process_id;
    info.creation_time = std::numeric_limits<unsigned long long>::max();
    info.protected_process =
        process_id == protected_process_id || process_id == current_process_id;
    unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                      FALSE, process_id));
    if (process) {
      FILETIME creation = {};
      FILETIME exit = {};
      FILETIME kernel = {};
      FILETIME user = {};
      if (GetProcessTimes(process.get(), &creation, &exit, &kernel, &user)) {
        info.creation_time = file_time_value(creation);
      }
    }
    processes.push_back(info);
  }

  std::sort(processes.begin(), processes.end(),
            [](const job_process_info_t &left,
               const job_process_info_t &right) {
              if (left.protected_process != right.protected_process) {
                return left.protected_process;
              }
              if (left.creation_time != right.creation_time) {
                return left.creation_time < right.creation_time;
              }
              return left.process_id < right.process_id;
            });

  unsigned long first_error = ERROR_SUCCESS;
  for (size_t index = maximum_processes; index < processes.size(); ++index) {
    const job_process_info_t &info = processes[index];
    if (info.protected_process) continue;
    unique_handle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                      info.process_id));
    if (! process) {
      const unsigned long open_error = GetLastError();
      if (open_error != ERROR_INVALID_PARAMETER && ! first_error) {
        first_error = open_error;
      }
      continue;
    }
    if (! TerminateProcess(process.get(), ERROR_PROCESS_ABORTED)) {
      if (! first_error) first_error = GetLastError();
      continue;
    }
    WaitForSingleObject(process.get(), 1000);
  }

  /*
    Process creation can race with the accounting snapshot. The job-wide
    assignment counter may briefly exceed the PID snapshot, and a process may
    exit or lose its handle while these entries are being reaped. The required
    invariant is the steady-state PID count, so retry the snapshot briefly
    before treating an individual termination failure as fatal.
  */
  for (unsigned long attempt = 0; attempt < 10; ++attempt) {
    if (attempt) Sleep(20);
    if (! query_job_process_ids(job, &process_ids, &assigned_processes,
                                error)) {
      return false;
    }
    if (process_ids.size() <= maximum_processes) {
      save_error(error, ERROR_SUCCESS);
      return true;
    }
  }

  save_error(error, first_error ? first_error : ERROR_BUSY);
  return false;
}

bool terminate_service_job(HANDLE job, unsigned long exit_code,
                           unsigned long *error) {
  if (! job) {
    save_error(error, ERROR_INVALID_HANDLE);
    return false;
  }
  if (! TerminateJobObject(job, exit_code)) {
    save_error(error, GetLastError());
    return false;
  }
  save_error(error, ERROR_SUCCESS);
  return true;
}

bool query_service_job(HANDLE job, job_accounting_t *accounting,
                       unsigned long *error) {
  if (! job || ! accounting) {
    save_error(error, ERROR_INVALID_PARAMETER);
    return false;
  }

  JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION basic = {};
  if (! QueryInformationJobObject(job, JobObjectBasicAndIoAccountingInformation,
                                  &basic, sizeof(basic), nullptr)) {
    save_error(error, GetLastError());
    return false;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended = {};
  if (! QueryInformationJobObject(job, JobObjectExtendedLimitInformation,
                                  &extended, sizeof(extended), nullptr)) {
    save_error(error, GetLastError());
    return false;
  }

  job_memory_usage_information_t memory = {};
  if (! QueryInformationJobObject(job, kJobObjectMemoryUsageInformation,
                                  &memory, sizeof(memory), nullptr)) {
    save_error(error, GetLastError());
    return false;
  }

  accounting->active_processes = basic.BasicInfo.ActiveProcesses;
  accounting->total_processes = basic.BasicInfo.TotalProcesses;
  accounting->terminated_processes = basic.BasicInfo.TotalTerminatedProcesses;
  accounting->user_time_100ns =
      static_cast<unsigned long long>(basic.BasicInfo.TotalUserTime.QuadPart);
  accounting->kernel_time_100ns =
      static_cast<unsigned long long>(basic.BasicInfo.TotalKernelTime.QuadPart);
  accounting->job_memory = memory.job_memory;
  accounting->peak_job_memory = extended.PeakJobMemoryUsed;
  accounting->peak_process_memory = extended.PeakProcessMemoryUsed;
  save_error(error, ERROR_SUCCESS);
  return true;
}

unsigned long calculate_job_cpu_percent(const job_accounting_t &previous,
                                        const job_accounting_t &current,
                                        unsigned long long elapsed_ms,
                                        unsigned long processor_count) {
  if (! elapsed_ms || ! processor_count) return 0;
  unsigned long long previous_cpu =
      previous.user_time_100ns + previous.kernel_time_100ns;
  unsigned long long current_cpu =
      current.user_time_100ns + current.kernel_time_100ns;
  if (current_cpu < previous_cpu) return 0;
  unsigned long long capacity = elapsed_ms * 10000ULL * processor_count;
  if (! capacity) return 0;
  unsigned long long percent = (current_cpu - previous_cpu) * 100ULL / capacity;
  return static_cast<unsigned long>(percent > 100 ? 100 : percent);
}
