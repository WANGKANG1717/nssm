#include "status_snapshot.h"

#include "config.h"
#include "recovery.h"
#include "service.h"
#include "win32_handle.h"

#include <sddl.h>

#include <cstring>
#include <string>
#include <type_traits>

namespace {

constexpr unsigned long kStatusMagic = 0x4D53534E;
constexpr unsigned long kStatusVersion = 1;
static_assert(std::is_trivially_copyable_v<status_snapshot_t>);

std::wstring mapping_name(const std::wstring &service_name) {
  return L"Global\\SvcPilot.Status." + service_name;
}

runtime_health_t current_health(const service_runtime_t *service,
                                bool health_degraded,
                                unsigned long health_successes) {
  if (! service || ! service->config) return runtime_health_t::unknown;
  if (! service->pid) return runtime_health_t::stopped;
  if (service->config->health.type == health_check_type_t::disabled) {
    return runtime_health_t::disabled;
  }
  if (health_degraded) return runtime_health_t::degraded;
  if (health_successes) return runtime_health_t::healthy;
  return runtime_health_t::starting;
}

void publish_status_snapshot_unlocked(service_runtime_t *service) {
  status_snapshot_t *snapshot = service->status_snapshot;
  if (! snapshot) return;
  InterlockedIncrement(&snapshot->sequence);
  MemoryBarrier();
  snapshot->magic = kStatusMagic;
  snapshot->version = kStatusVersion;
  snapshot->wrapper_pid = GetCurrentProcessId();
  snapshot->child_pid = service->pid;
  EnterCriticalSection(&service->health_section);
  bool health_degraded = service->health_degraded;
  unsigned long health_successes = service->health_successes;
  LeaveCriticalSection(&service->health_section);
  snapshot->health = static_cast<unsigned long>(
      current_health(service, health_degraded, health_successes));
  snapshot->restart_count = service->recovery_state
                                ? service->recovery_state->consecutive_restarts
                                : 0;
  EnterCriticalSection(&service->resource_section);
  snapshot->cpu_percent = service->resource_cpu_percent;
  snapshot->process_count = service->resource_processes;
  snapshot->memory_bytes = service->resource_memory_bytes;
  LeaveCriticalSection(&service->resource_section);
  snapshot->process_started_ms = service->process_started_ms;
  MemoryBarrier();
  InterlockedIncrement(&snapshot->sequence);
}

}  // namespace

bool create_status_snapshot(service_runtime_t *service) {
  if (! service || ! service->name[0]) return false;
  unique_local_memory<void> descriptor;
  if (! ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GR;;;AU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)",
          SDDL_REVISION_1, descriptor.put(), nullptr)) {
    return false;
  }
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor.get(),
                                    FALSE};
  std::wstring name = mapping_name(service->name);
  SetLastError(ERROR_SUCCESS);
  unique_handle mapping(CreateFileMappingW(
      INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
      sizeof(status_snapshot_t), name.c_str()));
  const unsigned long mapping_error = GetLastError();
  descriptor.reset();
  if (! mapping) {
    SetLastError(mapping_error);
    return false;
  }
  if (mapping_error == ERROR_ALREADY_EXISTS) {
    mapping.reset();
    SetLastError(ERROR_ALREADY_EXISTS);
    return false;
  }
  unique_mapped_view view(MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0,
                                        sizeof(status_snapshot_t)));
  if (! view) {
    const unsigned long error = GetLastError();
    mapping.reset();
    SetLastError(error);
    return false;
  }
  service->status_mapping = mapping.release();
  service->status_snapshot =
      static_cast<status_snapshot_t *>(view.release());
  ZeroMemory(service->status_snapshot, sizeof(status_snapshot_t));
  service->status_snapshot->magic = kStatusMagic;
  service->status_snapshot->version = kStatusVersion;
  publish_status_snapshot(service);
  return true;
}

void publish_status_snapshot(service_runtime_t *service) {
  if (! service) return;
  AcquireSRWLockExclusive(&service->status_snapshot_lock);
  publish_status_snapshot_unlocked(service);
  ReleaseSRWLockExclusive(&service->status_snapshot_lock);
}

void close_status_snapshot(service_runtime_t *service) {
  if (! service) return;
  AcquireSRWLockExclusive(&service->status_snapshot_lock);
  if (service->status_snapshot) {
    service->pid = 0;
    publish_status_snapshot_unlocked(service);
    UnmapViewOfFile(service->status_snapshot);
    service->status_snapshot = nullptr;
  }
  if (service->status_mapping) {
    CloseHandle(service->status_mapping);
    service->status_mapping = nullptr;
  }
  ReleaseSRWLockExclusive(&service->status_snapshot_lock);
}

bool read_status_snapshot(const std::wstring &service_name,
                          status_snapshot_t *snapshot, unsigned long *error) {
  if (! snapshot) {
    if (error) *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  unique_handle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE,
                                         mapping_name(service_name).c_str()));
  if (! mapping) {
    if (error) *error = GetLastError();
    return false;
  }
  unique_mapped_view view(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0,
                                        sizeof(status_snapshot_t)));
  if (! view) {
    if (error) *error = GetLastError();
    return false;
  }
  auto *shared = static_cast<const status_snapshot_t *>(view.get());
  bool copied = false;
  for (unsigned int attempt = 0; attempt < 8; ++attempt) {
    long before = shared->sequence;
    if (before & 1) {
      SwitchToThread();
      continue;
    }
    MemoryBarrier();
    memcpy(snapshot, shared, sizeof(*snapshot));
    MemoryBarrier();
    long after = shared->sequence;
    if (before == after && !(after & 1) && snapshot->magic == kStatusMagic &&
        snapshot->version == kStatusVersion) {
      copied = true;
      break;
    }
  }
  if (error) *error = copied ? ERROR_SUCCESS : ERROR_RETRY;
  return copied;
}

const wchar_t *runtime_health_text(runtime_health_t health) {
  switch (health) {
    case runtime_health_t::disabled: return L"disabled";
    case runtime_health_t::starting: return L"starting";
    case runtime_health_t::healthy: return L"healthy";
    case runtime_health_t::degraded: return L"degraded";
    case runtime_health_t::stopped: return L"stopped";
    case runtime_health_t::unknown:
    default: return L"unknown";
  }
}
