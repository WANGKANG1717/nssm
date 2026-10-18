# Repository audit

Audit date: 2026-10-19  
Baseline: Windows 10+, x64, C++20, MinGW-w64 GCC 14+, Unicode only, CLI only.

This report records the state after the GUI, Visual Studio projects, registry-style
configuration commands, ANSI abstractions and unsupported Windows compatibility
paths were removed. Priority is defined as P0 (security/undefined behaviour), P1
(lifecycle/data loss/error loss), P2 (high-value structure/duplication/legacy),
and P3 (low-risk maintainability).

## Module and dependency findings

| Location | Type | Current state and risk | Recommendation | Effort | Priority | Test protection |
|---|---|---|---|---|---|---|
| `src/supervisor/runtime/service.cpp` | module boundary | Reduced from 874 to 788 lines. State allocation/destruction and process-wait registration now have dedicated modules. Launch, controls, stop and recovery still share one file. | Extract launch and stop/recovery state machines only when an administrator-capable lifecycle harness is available. | M | P2 | Required |
| `src/supervisor/runtime/nssm_io.cpp` | module boundary | 709 lines; pipe setup, offline rotation, online rotation, encoding and timestamping remain coupled. | Split pipe ownership from log transformation and rotation policy. | M | P2 | Required |
| `src/config/config.cpp` | module boundary | 707 lines, but its single responsibility is JSON-to-model parsing and validation. | Split by configuration section if schema growth resumes; do not split only to reduce line count. | S | P3 | Existing unit tests |
| `src/app`, `src/cli` | dependency direction | CLI no longer opens SCM or registry keys directly. App contains dispatch/elevation only. | Keep SCM/config operations behind supervisor/config APIs. | S | P3 | Compile gate |
| public runtime headers | encapsulation | `service.h` exposes the complete runtime structure to monitoring and I/O modules. | Replace direct field access with smaller runtime views after lifecycle tests exist. | L | P2 | Required |

No `platform -> cli/ui`, `config -> cli`, GUI resource, Visual Studio project or
umbrella `nssm.h` dependency remains. The only source files above 700 lines are
`service.cpp` (788), `nssm_io.cpp` (709) and `config.cpp` (707). Long functions
still requiring later decomposition are `get_output_handles`, `log_and_rotate`,
`service_main` and `end_service`.

## Global state and resource ownership

| Location/state | Creator and owner | Destruction/threading | Risk | Recommendation | Priority | Test protection |
|---|---|---|---|---|---|---|
| `application_paths()` immutable cache | app startup; process lifetime | function-local immutable static | Low | Retain. | P3 | Version/CLI tests |
| event error buffer | first use on each thread | `thread_local` vector, runtime-managed | Low; memory retained until thread exit | Retain; it prevents cross-thread formatting races. | P3 | Unit tests |
| `nssm_service_t` | `service_main`; service runtime | `service_state.cpp`; lifecycle, health and resource locks | Normal service-process teardown requires SCM integration. | Retain the administrator CI lifecycle test and extend it with fault injection. | P2 | GitHub SCM test |
| child process/job/wait handles | service launch/runtime | lifecycle lock plus `service_process_wait.cpp` | Ownership is now explicit; callback unregistration must remain outside lifecycle-lock waits. | Retain ordering and document it in tests. | P1 | GitHub SCM test |
| health/resource threads and events | monitor start functions | monitor stop functions under dedicated locks | Manual handles are long-lived fields, but failure rollback and join-before-close are present. | Convert only if the runtime object itself becomes RAII-owned. | P2 | Partial |
| status mapping/view | status publisher | `close_status_snapshot`; snapshot SRW lock | Reader consistency uses an odd/even sequence protocol. | Add multi-process stress coverage. | P2 | Unit coverage only |
| online rotation state | control handler/logger/shutdown | shared scalar fields | Concurrent reads/writes are not synchronized. | Replace with an interlocked/atomic state protocol and stress rotation during stop. | P1 | Required |
| logger retry delays | logger thread/service shutdown | ordinary `Sleep` followed by cleanup's infinite join | Cancelling synchronous I/O cannot wake a disk-full/quota retry sleep, so stop can be delayed for tens of seconds. | Make retry backoff observe the service stop event. | P1 | Required |
| debug privilege | process-tree query scope | `debug_privilege_guard` enables the privilege on the active thread token and restores both privilege and impersonation state. | Retain the scoped platform RAII implementation. | Done | Unit and SCM tests |

No mutable process-wide global variable was found. Win32 transient handles in
SCM, registry, process query, mapping and configuration code use the wrappers in
`platform/win32_handle.h`. Long-lived handles intentionally remain runtime fields.

## Duplicate code and rule drift

| Location | Duplicate class | Behaviour drift / synchronization cost | Recommendation | Effort | Priority | Test protection |
|---|---|---|---|---|---|---|
| offline and online rotation in `nssm_io.cpp` | structural/semantic | Filename, copy/truncate and reopen rules are implemented twice and already have different failure recovery. | Extract a rotation transaction after the online-state race is fixed. | M | P2 | Required |
| SCM open/query sequences in config and supervisor | structural | Error mapping and desired-access selection are repeated, but belong to distinct operations. | Introduce a small SCM session helper only when another operation is added. | S | P3 | Existing CLI tests |
| ACL path and handle evaluation | structural | Both now share `secure_acl`; target-object checks no longer duplicate ACL rules. | Retain the shared evaluator. | Done | P1 | Unit tests |
| configuration validation | semantic | CLI validation, plan/apply and runtime now use one parser/model. Secure operations additionally bind ACL audit and read to the same handle. | Retain the two explicit load modes. | Done | P1 | Unit/CLI tests |
| status/enum text mappings | generated-style | Small switch tables are separate and stable. | Do not introduce code generation for these small sets. | None | P3 | Compile gate |

## Legacy and compatibility disposition

| Location | Classification | Disposition | Reason / remaining action | Priority | Test protection |
|---|---|---|---|---|---|
| GUI and GUI resource IDs | delete | Deleted | A future web application can consume CLI/JSON/status APIs without coupling core supervision to controls. | Done | Build gate |
| VS2008 `.sln`/`.vcproj`, MSVC `/MT` | delete | Deleted | MinGW-w64 GCC is the only supported toolchain. | Done | CMake configure gate |
| x86 and pre-Windows-10 branches | delete | Deleted | x64 and `_WIN32_WINNT=0x0A00` are enforced. | Done | CMake configure gate |
| `TCHAR`, `_T()`, generic Win32 API names | replace | Replaced with `wchar_t`, wide formatting and explicit `...W` APIs | Removes a permanently dead ANSI abstraction. | Done | Debug/Release tests |
| display-name service lookup | delete | Deleted | Commands now require exact SCM service names. | Done | CLI/integration tests |
| interactive-service handling | delete | Deleted | Session 0 makes it unsupported on the declared baseline. | Done | Build gate |
| `pause` CLI command | delete | Deleted; the defensive control-handler branch remains because Windows shares the PAUSE/CONTINUE acceptance mask. | `continue` remains available to wake a recovery delay. | Done | CLI/build tests |
| raw heap environment blocks | preserve | Required by Win32 environment-block APIs and owned by the runtime object. | Wrap only as part of a runtime-object RAII redesign. | P3 | Child-process tests |

## Release gate

Debug and Release builds each pass the five current tests, and GCC `-fanalyzer`
builds without diagnostics. Local SCM integration requires elevation and remains
a GitHub Actions gate. A release tag must not be created while either the online
rotation race or the non-interruptible logger retry delay above is open.

## Product gaps after the audit

Compared with WinSW, Servy, AlwaysUp and systemd-style supervision, the highest
value remaining capabilities are:

1. secure user-account credentials through Credential Manager/DPAPI, including rotation;
2. structured logs, retention/compression, disk quotas and back-pressure behaviour;
3. native metrics/tracing and a stable local management API for a future web UI;
4. live reconcile/reload, drift detection and transactional multi-service apply;
5. stronger sandbox provisioning: restricted tokens, service-SID ACL grants and privilege allowlists;
6. richer dependency ordering, restart propagation, timer/path/socket activation and one-shot jobs;
7. signed MSI/winget delivery, Authenticode signing, upgrade/rollback and provenance verification;
8. schema publication, shell completion, config migration tooling and policy linting;
9. administrator SCM chaos tests, rotation/load stress tests, fuzzing and fault injection;
10. fleet-oriented remote control/RBAC/audit history, implemented above the lightweight core rather than inside it.
