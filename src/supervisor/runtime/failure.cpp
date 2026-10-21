#include "failure.h"

#include "service.h"

namespace {

template <size_t Size>
void copy_text(wchar_t (&destination)[Size], const wchar_t *source) {
  _snwprintf_s(destination, Size, _TRUNCATE, L"%ls", source ? source : L"");
}

}  // namespace

const wchar_t *failure_kind_text(failure_kind_t kind) {
  switch (kind) {
    case failure_kind_t::none: return L"none";
    case failure_kind_t::process_exit: return L"processExit";
    case failure_kind_t::start_failure: return L"startFailure";
    case failure_kind_t::readiness: return L"readiness";
    case failure_kind_t::health: return L"health";
    case failure_kind_t::watchdog: return L"watchdog";
    case failure_kind_t::resource_memory: return L"resourceMemory";
    case failure_kind_t::resource_cpu: return L"resourceCpu";
    case failure_kind_t::resource_processes: return L"resourceProcesses";
    case failure_kind_t::lifecycle: return L"lifecycle";
    case failure_kind_t::restart_rate_limit: return L"restartRateLimit";
    case failure_kind_t::internal: return L"internal";
  }
  return L"internal";
}

failure_kind_t parse_failure_kind(const std::wstring &text) {
  for (unsigned long value = static_cast<unsigned long>(failure_kind_t::none);
       value <= static_cast<unsigned long>(failure_kind_t::internal); ++value) {
    const auto kind = static_cast<failure_kind_t>(value);
    if (text == failure_kind_text(kind)) return kind;
  }
  return failure_kind_t::internal;
}

void reset_runtime_failure(service_runtime_t *service) {
  if (! service) return;
  AcquireSRWLockExclusive(&service->failure_lock);
  service->failure_kind = failure_kind_t::none;
  service->failure_phase[0] = L'\0';
  service->failure_exit_code = 0;
  service->failure_windows_error = 0;
  service->failure_message[0] = L'\0';
  service->failure_restart_attempts = 0;
  ReleaseSRWLockExclusive(&service->failure_lock);
}

bool record_runtime_failure(service_runtime_t *service, failure_kind_t kind,
                            const wchar_t *phase, unsigned long exit_code,
                            unsigned long windows_error,
                            const std::wstring &message,
                            unsigned long restart_attempts) {
  if (! service || kind == failure_kind_t::none) return false;
  AcquireSRWLockExclusive(&service->failure_lock);
  const bool recorded = service->failure_kind == failure_kind_t::none;
  if (recorded) {
    service->failure_kind = kind;
    copy_text(service->failure_phase, phase);
    service->failure_exit_code = exit_code;
    service->failure_windows_error = windows_error;
    copy_text(service->failure_message, message.c_str());
    service->failure_restart_attempts = restart_attempts;
  }
  ReleaseSRWLockExclusive(&service->failure_lock);
  return recorded;
}

failure_record_t runtime_failure(const service_runtime_t *service) {
  failure_record_t result;
  if (! service) return result;
  auto *mutable_service = const_cast<service_runtime_t *>(service);
  AcquireSRWLockShared(&mutable_service->failure_lock);
  result.kind = service->failure_kind;
  result.phase = service->failure_phase;
  result.exit_code = service->failure_exit_code;
  result.windows_error = service->failure_windows_error;
  result.message = service->failure_message;
  result.restart_attempts = service->failure_restart_attempts;
  ReleaseSRWLockShared(&mutable_service->failure_lock);
  return result;
}
