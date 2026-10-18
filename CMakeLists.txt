cmake_minimum_required(VERSION 3.25)

project(nssm VERSION 3.0.0 LANGUAGES CXX RC)

set(NSSM_BUILD_LABEL "release" CACHE STRING
    "Stable label shown by 'nssm version' (avoid timestamps for reproducible builds)")

if(NOT WIN32)
  message(FATAL_ERROR "NSSM can only be built for Windows")
endif()
if(NOT MINGW OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "NSSM supports only MinGW-w64 GCC")
endif()

if(CMAKE_SIZEOF_VOID_P LESS 8)
  message(FATAL_ERROR "NSSM supports only 64-bit builds")
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

nssm_compile_messages(
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/messages.mc"
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
          -InputPath "${CMAKE_CURRENT_SOURCE_DIR}/nssm.rc"
          -OutputPath "${NSSM_RESOURCE_SOURCE}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/nssm.rc"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConvertResource.ps1"
  COMMENT "Escaping UTF-8 dialog resources for windres"
  VERBATIM
)

set(NSSM_SOURCES
  account.cpp
  child_process.cpp
  console.cpp
  env.cpp
  event.cpp
  gui.cpp
  hook.cpp
  imports.cpp
  nssm_io.cpp
  nssm.cpp
  nssm.manifest
  process.cpp
  registry.cpp
  service.cpp
  settings.cpp
  utf8.cpp
  "${NSSM_RESOURCE_SOURCE}"
)

add_executable(nssm ${NSSM_SOURCES})
add_dependencies(nssm nssm_messages)

set_property(
  SOURCE "${NSSM_RESOURCE_SOURCE}"
  APPEND PROPERTY OBJECT_DEPENDS
    "${NSSM_MESSAGES_HEADER};${NSSM_MESSAGES_RESOURCE};${NSSM_MESSAGES_BINARIES}"
)

# The generated script is ASCII-only and contains explicit Unicode escapes,
# so windres no longer depends on the host's active Windows code page.
set_source_files_properties("${NSSM_RESOURCE_SOURCE}" PROPERTIES GENERATED TRUE)
set_property(SOURCE "${NSSM_RESOURCE_SOURCE}" APPEND PROPERTY
             COMPILE_OPTIONS "--codepage=65001")

target_include_directories(nssm PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_definitions(nssm PRIVATE
  UNICODE
  _UNICODE
  NSSM_BUILD_LABEL="${NSSM_BUILD_LABEL}"
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
  NTDDI_VERSION=0x0A000000
)

nssm_configure_compiler(nssm)

target_link_libraries(nssm PRIVATE
  advapi32
  comctl32
  comdlg32
  gdi32
  ole32
  psapi
  shell32
  shlwapi
  user32
  uuid
)

set_target_properties(nssm PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/out/${CMAKE_BUILD_TYPE}/win64"
)

include(CTest)

if(BUILD_TESTING)
  add_test(NAME nssm.version COMMAND $<TARGET_FILE:nssm> version)
  set_tests_properties(nssm.version PROPERTIES
    PASS_REGULAR_EXPRESSION "NSSM ${PROJECT_VERSION} 64-bit"
  )

  add_test(
    NAME nssm.resources
    COMMAND "${NSSM_RESOURCE_POWERSHELL_EXECUTABLE}"
            -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyResources.ps1"
            -Executable $<TARGET_FILE:nssm>
  )

  add_executable(nssm_unit_tests
    tests/child_process_tests.cpp
    child_process.cpp
  )
  target_compile_definitions(nssm_unit_tests PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
  )
  nssm_configure_compiler(nssm_unit_tests)
  add_test(NAME nssm.unit COMMAND $<TARGET_FILE:nssm_unit_tests>)
endif()

