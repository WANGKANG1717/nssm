cmake_minimum_required(VERSION 3.25)

project(svcpilot VERSION 3.2.4 LANGUAGES C CXX RC)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/app/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
  @ONLY
)

set(NSSM_BUILD_LABEL "release" CACHE STRING
    "Stable label shown by 'svp version' (avoid timestamps for reproducible builds)")

if(NOT WIN32)
  message(FATAL_ERROR "SvcPilot can only be built for Windows")
endif()
if(NOT MINGW OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "SvcPilot supports only MinGW-w64 GCC")
endif()

if(CMAKE_SIZEOF_VOID_P LESS 8)
  message(FATAL_ERROR "SvcPilot supports only 64-bit builds")
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "MinGW-w64 GCC 14 or newer is required")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(CompileMessages)
include(FetchContent)

FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

nssm_compile_messages(
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/resources/messages.mc"
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated"
  OUT_HEADER NSSM_MESSAGES_HEADER
  OUT_RESOURCE NSSM_MESSAGES_RESOURCE
  OUT_BINARIES NSSM_MESSAGES_BINARIES
)

set(NSSM_RESOURCE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/nssm.rc")
find_program(NSSM_RESOURCE_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
add_custom_command(
  OUTPUT "${NSSM_RESOURCE_SOURCE}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND "${NSSM_RESOURCE_POWERSHELL_EXECUTABLE}"
          -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
          -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConvertResource.ps1"
          -InputPath "${CMAKE_CURRENT_SOURCE_DIR}/resources/nssm.rc"
          -OutputPath "${NSSM_RESOURCE_SOURCE}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/nssm.rc"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConvertResource.ps1"
  COMMENT "Escaping UTF-8 executable resources for windres"
  VERBATIM
)

set(NSSM_APP_SOURCES
  src/app/nssm.cpp
)

set(NSSM_CLI_SOURCES
  src/cli/config_commands.cpp
  src/cli/logs.cpp
  src/cli/output.cpp
  src/cli/service_commands.cpp
  src/cli/status.cpp
  src/cli/usage.cpp
)

set(NSSM_CONFIG_SOURCES
  src/config/config.cpp
  src/config/config_loader.cpp
  src/config/config_parse_utils.cpp
  src/config/config_registration.cpp
  src/config/config_security.cpp
  src/config/config_service.cpp
)

set(NSSM_SUPERVISOR_SOURCES
  src/supervisor/process/job_object.cpp
  src/supervisor/process/process.cpp
  src/supervisor/monitoring/health_check.cpp
  src/supervisor/monitoring/health_monitor.cpp
  src/supervisor/monitoring/notification.cpp
  src/supervisor/monitoring/readiness.cpp
  src/supervisor/monitoring/resource_monitor.cpp
  src/supervisor/monitoring/status_snapshot.cpp
  src/supervisor/monitoring/status_query.cpp
  src/supervisor/runtime/nssm_io.cpp
  src/supervisor/runtime/recovery.cpp
  src/supervisor/runtime/service.cpp
  src/supervisor/runtime/service_config.cpp
  src/supervisor/runtime/service_control.cpp
  src/supervisor/runtime/service_enumeration.cpp
  src/supervisor/runtime/service_operations.cpp
  src/supervisor/runtime/service_process_wait.cpp
  src/supervisor/runtime/service_state.cpp
  src/supervisor/runtime/service_wait.cpp
)

set(NSSM_PLATFORM_SOURCES
  src/platform/child_process.cpp
  src/platform/command_runner.cpp
  src/platform/console.cpp
  src/platform/credential.cpp
  src/platform/env.cpp
  src/platform/event.cpp
  src/platform/registry.cpp
  src/platform/security.cpp
  src/platform/service_status.cpp
  src/platform/string_utils.cpp
  src/platform/utf8.cpp
)

set(NSSM_SOURCES
  ${NSSM_APP_SOURCES}
  ${NSSM_CLI_SOURCES}
  ${NSSM_CONFIG_SOURCES}
  ${NSSM_SUPERVISOR_SOURCES}
  ${NSSM_PLATFORM_SOURCES}
  resources/nssm.manifest
  "${NSSM_RESOURCE_SOURCE}"
)

add_executable(svp ${NSSM_SOURCES})
add_dependencies(svp nssm_messages)

set_property(
  SOURCE "${NSSM_RESOURCE_SOURCE}"
  APPEND PROPERTY OBJECT_DEPENDS
    "${NSSM_MESSAGES_HEADER};${NSSM_MESSAGES_RESOURCE};${NSSM_MESSAGES_BINARIES};${CMAKE_CURRENT_BINARY_DIR}/generated/version.h;${CMAKE_CURRENT_SOURCE_DIR}/resources/resource.h;${CMAKE_CURRENT_SOURCE_DIR}/resources/nssm.ico;${CMAKE_CURRENT_SOURCE_DIR}/resources/nssm.manifest"
)

# The generated script is ASCII-only and contains explicit Unicode escapes,
# so windres no longer depends on the host's active Windows code page.
set_source_files_properties("${NSSM_RESOURCE_SOURCE}" PROPERTIES GENERATED TRUE)
set_property(SOURCE "${NSSM_RESOURCE_SOURCE}" APPEND PROPERTY
             COMPILE_OPTIONS "--codepage=65001")

target_include_directories(svp PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cli"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/config"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/supervisor/process"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/supervisor/monitoring"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/supervisor/runtime"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_definitions(svp PRIVATE
  UNICODE
  _UNICODE
  NSSM_BUILD_LABEL="${NSSM_BUILD_LABEL}"
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
  NTDDI_VERSION=0x0A000000
)

nssm_configure_compiler(svp)

target_link_libraries(svp PRIVATE
  advapi32
  crypt32
  shell32
  shlwapi
  user32
  winhttp
  ws2_32
  tomlplusplus::tomlplusplus
)

set_target_properties(svp PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/out/${CMAKE_BUILD_TYPE}/win64"
)

include(CTest)

if(BUILD_TESTING)
  add_test(NAME svp.version COMMAND $<TARGET_FILE:svp> version)
  set_tests_properties(svp.version PROPERTIES
    PASS_REGULAR_EXPRESSION "SvcPilot ${PROJECT_VERSION} 64-bit"
  )

  add_test(
    NAME svp.usage
    COMMAND "${NSSM_RESOURCE_POWERSHELL_EXECUTABLE}"
            -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyUsage.ps1"
            -Executable $<TARGET_FILE:svp>
            -ExpectedVersion "${PROJECT_VERSION}"
  )

  add_test(
    NAME svp.validate
    COMMAND $<TARGET_FILE:svp> validate
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/minimal-service.toml"
  )
  set_tests_properties(svp.validate PROPERTIES
    PASS_REGULAR_EXPRESSION "Configuration is valid: svcpilot-config-test"
  )

  add_test(
    NAME svp.resources
    COMMAND "${NSSM_RESOURCE_POWERSHELL_EXECUTABLE}"
            -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyResources.ps1"
            -Executable $<TARGET_FILE:svp>
            -ExpectedVersion "${PROJECT_VERSION}"
  )

  add_executable(svp_unit_tests
    tests/child_process_tests.cpp
    src/cli/output.cpp
    src/platform/child_process.cpp
    src/platform/command_runner.cpp
    src/platform/console.cpp
    src/config/config.cpp
    src/config/config_parse_utils.cpp
    src/config/config_security.cpp
    src/supervisor/monitoring/health_check.cpp
    src/supervisor/process/job_object.cpp
    src/supervisor/monitoring/readiness.cpp
    src/supervisor/monitoring/notification.cpp
    src/supervisor/runtime/recovery.cpp
    src/supervisor/runtime/nssm_io.cpp
    src/platform/event.cpp
    src/platform/security.cpp
    src/platform/service_status.cpp
    src/platform/string_utils.cpp
    src/platform/utf8.cpp
  )
  add_dependencies(svp_unit_tests nssm_messages)
  target_include_directories(svp_unit_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/config"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cli"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/supervisor/process"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/supervisor/monitoring"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/supervisor/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  )
  target_compile_definitions(svp_unit_tests PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
  )
  nssm_configure_compiler(svp_unit_tests)
  target_link_libraries(svp_unit_tests PRIVATE advapi32 shlwapi winhttp ws2_32 tomlplusplus::tomlplusplus)
  add_test(NAME svp.unit COMMAND $<TARGET_FILE:svp_unit_tests>)
  add_test(
    NAME svp.validate-json
    COMMAND $<TARGET_FILE:svp> validate --output=json
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/minimal-service.toml"
  )
  set_tests_properties(svp.validate-json PROPERTIES
    PASS_REGULAR_EXPRESSION "\"ok\":true.*\"operation\":\"validate\""
  )
endif()

