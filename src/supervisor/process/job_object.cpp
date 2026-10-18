#include "job_object.h"
#include "win32_handle.h"

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

void save_error(unsigned long *error, unsigned long value) {
  if (error) *error = value;
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
