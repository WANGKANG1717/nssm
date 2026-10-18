cmake_minimum_required(VERSION 3.25)

project(nssm VERSION 3.0.0 LANGUAGES CXX RC)

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

set(NSSM_SOURCES
  account.cpp
  console.cpp
  env.cpp
  event.cpp
  gui.cpp
  hook.cpp
  imports.cpp
  nssm_io.cpp
  nssm.cpp
  process.cpp
  registry.cpp
  service.cpp
  settings.cpp
  utf8.cpp
  nssm.rc
)

add_executable(nssm ${NSSM_SOURCES})
add_dependencies(nssm nssm_messages)

set_property(
  SOURCE nssm.rc
  APPEND PROPERTY OBJECT_DEPENDS
    "${NSSM_MESSAGES_HEADER};${NSSM_MESSAGES_RESOURCE};${NSSM_MESSAGES_BINARIES}"
)

target_include_directories(nssm PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_definitions(nssm PRIVATE
  UNICODE
  _UNICODE
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
endif()

