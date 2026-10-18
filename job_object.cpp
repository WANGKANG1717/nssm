#include "job_object.h"

namespace {

void save_error(unsigned long *error, unsigned long value) {
  if (error) *error = value;
}

}  // namespace

HANDLE create_service_job(unsigned long *error) {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (! job) {
    save_error(error, GetLastError());
    return nullptr;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (! SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits))) {
    save_error(error, GetLastError());
    CloseHandle(job);
    return nullptr;
  }

  save_error(error, ERROR_SUCCESS);
  return job;
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

  accounting->active_processes = basic.BasicInfo.ActiveProcesses;
  accounting->total_processes = basic.BasicInfo.TotalProcesses;
  accounting->terminated_processes = basic.BasicInfo.TotalTerminatedProcesses;
  accounting->user_time_100ns =
      static_cast<unsigned long long>(basic.BasicInfo.TotalUserTime.QuadPart);
  accounting->kernel_time_100ns =
      static_cast<unsigned long long>(basic.BasicInfo.TotalKernelTime.QuadPart);
  accounting->peak_job_memory = extended.PeakJobMemoryUsed;
  accounting->peak_process_memory = extended.PeakProcessMemoryUsed;
  save_error(error, ERROR_SUCCESS);
  return true;
}
