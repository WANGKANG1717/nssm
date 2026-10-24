#ifndef JOB_OBJECT_H
#define JOB_OBJECT_H

#include <windows.h>

struct job_accounting_t {
  unsigned long active_processes;
  unsigned long total_processes;
  unsigned long terminated_processes;
  unsigned long long user_time_100ns;
  unsigned long long kernel_time_100ns;
  unsigned long long job_memory;
  size_t peak_job_memory;
  size_t peak_process_memory;
};

/* Create an unnamed job which terminates all members when its handle closes. */
HANDLE create_service_job(unsigned long *error);

bool configure_service_job_limits(HANDLE job,
                                  unsigned long maximum_processes,
                                  unsigned long long memory_limit_bytes,
                                  unsigned long *error);

bool assign_process_to_service_job(HANDLE job, HANDLE process,
                                   unsigned long *error);
bool enforce_service_job_process_limit(HANDLE job,
                                       unsigned long maximum_processes,
                                       unsigned long protected_process_id,
                                       unsigned long *error);
bool terminate_service_job(HANDLE job, unsigned long exit_code,
                           unsigned long *error);
bool query_service_job(HANDLE job, job_accounting_t *accounting,
                       unsigned long *error);
unsigned long calculate_job_cpu_percent(const job_accounting_t &previous,
                                        const job_accounting_t &current,
                                        unsigned long long elapsed_ms,
                                        unsigned long processor_count);

#endif
