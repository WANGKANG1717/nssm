include(CMakeParseArguments)

function(nssm_compile_messages)
  set(options)
  set(one_value_args SOURCE OUTPUT_DIR OUT_HEADER OUT_RESOURCE OUT_BINARIES)
  cmake_parse_arguments(MSG "${options}" "${one_value_args}" "" ${ARGN})

  foreach(required_arg SOURCE OUTPUT_DIR OUT_HEADER OUT_RESOURCE OUT_BINARIES)
    if(NOT MSG_${required_arg})
      message(FATAL_ERROR "nssm_compile_messages requires ${required_arg}")
    endif()
  endforeach()

  find_program(NSSM_WINDMC_EXECUTABLE NAMES windmc REQUIRED)
  find_program(NSSM_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)

  set(converted_source "${MSG_OUTPUT_DIR}/messages.mc")
  set(generated_header "${MSG_OUTPUT_DIR}/messages.h")
  set(generated_resource "${MSG_OUTPUT_DIR}/messages.rc")
  set(generated_binaries
    "${MSG_OUTPUT_DIR}/MSG00409.bin"
    "${MSG_OUTPUT_DIR}/MSG00804.bin"
  )

  add_custom_command(
    OUTPUT
      "${generated_header}"
      "${generated_resource}"
      ${generated_binaries}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MSG_OUTPUT_DIR}"
    COMMAND
      "${NSSM_POWERSHELL_EXECUTABLE}"
      -NoLogo
      -NoProfile
      -NonInteractive
      -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConvertMessages.ps1"
      -InputPath "${MSG_SOURCE}"
      -OutputPath "${converted_source}"
    COMMAND
      "${NSSM_WINDMC_EXECUTABLE}"
      -u
      -U
      -h "${MSG_OUTPUT_DIR}"
      -r "${MSG_OUTPUT_DIR}"
      "${converted_source}"
    DEPENDS
      "${MSG_SOURCE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConvertMessages.ps1"
    WORKING_DIRECTORY "${MSG_OUTPUT_DIR}"
    COMMENT "Compiling UTF-8 message catalog with windmc"
    VERBATIM
  )

  add_custom_target(nssm_messages DEPENDS
    "${generated_header}"
    "${generated_resource}"
    ${generated_binaries}
  )

  set(${MSG_OUT_HEADER} "${generated_header}" PARENT_SCOPE)
  set(${MSG_OUT_RESOURCE} "${generated_resource}" PARENT_SCOPE)
  set(${MSG_OUT_BINARIES} "${generated_binaries}" PARENT_SCOPE)
endfunction()
