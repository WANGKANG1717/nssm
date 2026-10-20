#include "child_process.h"
#include "config_commands.h"
#include "console.h"
#include "event.h"
#include "messages.h"
#include "logs.h"
#include "output.h"
#include "registry.h"
#include "security.h"
#include "service.h"
#include "service_constants.h"
#include "service_commands.h"
#include "status.h"
#include "string_utils.h"
#include "config.h"
#include "utf8.h"
#include "usage.h"
#include "version.h"
#include "windows_limits.h"

#include <shellapi.h>
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct application_paths_t {
  std::wstring unquoted;
  std::wstring quoted;
  unsigned long error = ERROR_SUCCESS;
};

application_paths_t load_application_paths() {
  application_paths_t paths;
  std::vector<wchar_t> buffer(PATH_LENGTH);
  SetLastError(ERROR_SUCCESS);
  const unsigned long length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<unsigned long>(buffer.size()));
  if (! length || length >= buffer.size()) {
    const unsigned long path_error = GetLastError();
    paths.error = path_error == ERROR_SUCCESS ? ERROR_INSUFFICIENT_BUFFER
                                              : path_error;
    return paths;
  }
  paths.unquoted.assign(buffer.data(), length);
  paths.quoted = quote_windows_argument(paths.unquoted);
  return paths;
}

const application_paths_t &application_paths() {
  static const application_paths_t paths = load_application_paths();
  return paths;
}

}  // namespace

static const wchar_t *svp_unquoted_imagepath();
static const wchar_t *svp_imagepath();

/* User requested us to print our version. */
static bool is_version(const wchar_t *s) {
  if (! s || ! *s) return false;
  /* /version */
  if (*s == '/') s++;
  else if (*s == '-') {
    /* -v, -V, -version, --version */
    s++;
    if (*s == '-') s++;
    else if (strings_equal_ignore_case(s, L"v")) return true;
  }
  if (strings_equal_ignore_case(s, L"version")) return true;
  return false;
}

static int elevate(int argc, wchar_t **argv, unsigned long message) {
  print_message(stderr, message);

  std::vector<std::wstring> arguments;
  arguments.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  std::wstring parameters = join_windows_arguments(arguments);

  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof(sei);
  sei.lpVerb = L"runas";
  sei.lpFile = svp_unquoted_imagepath();
  sei.lpParameters = parameters.c_str();
  sei.nShow = SW_SHOW;

  return ShellExecuteExW(&sei) ? 0 : 100;
}

static const wchar_t *svp_unquoted_imagepath() {
  return application_paths().unquoted.c_str();
}

static const wchar_t *svp_imagepath() {
  return application_paths().quoted.c_str();
}

int wmain(int argc, wchar_t **argv) {
  if (console_attached()) setup_utf8();

  /* Remember our path for later. */
  const application_paths_t &paths = application_paths();
  if (paths.error != ERROR_SUCCESS) {
    fwprintf(stderr, L"Unable to resolve the SvcPilot executable path: %ls\n",
             error_string(paths.error));
    return 100;
  }

  const bool is_admin = is_process_administrator();

  /* Elevate */
  if (argc > 1) {
    /*
      Valid commands are:
      validate, plan, apply, export, start, stop, restart, continue, remove,
      status, show, is-active, is-failed, reset-failed, enable, disable, mask, unmask, rotate,
      list, list-units, processes, version
    */
    if (strings_equal_ignore_case(argv[1], L"--help") ||
        strings_equal_ignore_case(argv[1], L"-h")) return usage(0);
    if (is_version(argv[1])) {
      wprintf(L"%ls %ls %ls %ls\n", kProductName, SVP_VERSION,
               SVP_CONFIGURATION, SVP_DATE);
      return 0;
    }
    if (strings_equal_ignore_case(argv[1], L"validate")) {
      return validate_service_config_command(argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"plan")) {
      return plan_service_config_command(argc - 2, argv + 2,
                                         svp_imagepath());
    }
    if (strings_equal_ignore_case(argv[1], L"export")) {
      return export_service_config_command(argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"apply")) {
      if (! is_admin) {
        cli_arguments_t arguments = parse_cli_arguments(argc - 2, argv + 2);
        if (arguments.json) {
          print_cli_error(true, L"permission",
                          L"Administrator access is required to apply a service configuration");
          return static_cast<int>(cli_exit_code::permission);
        }
        return elevate(argc, argv,
                       SVP_MESSAGE_NOT_ADMINISTRATOR_CANNOT_INSTALL);
      }
      create_messages(svp_unquoted_imagepath());
      return apply_service_config_command(argc - 2, argv + 2,
                                          svp_imagepath());
    }
    if (strings_equal_ignore_case(argv[1], L"credential")) {
      if (! is_admin) return elevate(argc, argv, SVP_MESSAGE_NOT_ADMINISTRATOR_CANNOT_INSTALL);
      return credential_command(argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"logs")) {
      return logs_command(argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"start")) return control_services_command(kServiceControlStart, false, argc - 2, argv + 2);
    if (strings_equal_ignore_case(argv[1], L"stop")) return control_services_command(SERVICE_CONTROL_STOP, false, argc - 2, argv + 2);
    if (strings_equal_ignore_case(argv[1], L"restart")) {
      return control_services_command(0, true, argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"reload")) {
      return reload_services_command(false, argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"reload-or-restart")) {
      return reload_services_command(true, argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"continue")) return control_service(SERVICE_CONTROL_CONTINUE, argc - 2, argv + 2);
    if (strings_equal_ignore_case(argv[1], L"status")) {
      return inspect_service_status_command(argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"rotate")) return control_service(kServiceControlRotate, argc - 2, argv + 2);
    if (strings_equal_ignore_case(argv[1], L"list") ||
        strings_equal_ignore_case(argv[1], L"list-units")) {
      return list_services(argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"show")) return show_service_command(argc - 2, argv + 2);
    if (strings_equal_ignore_case(argv[1], L"is-active")) return is_active_command(argc - 2, argv + 2);
    if (strings_equal_ignore_case(argv[1], L"is-failed")) return is_failed_command(argc - 2, argv + 2);
    if (strings_equal_ignore_case(argv[1], L"reset-failed")) {
      if (! is_admin) return elevate(argc, argv, SVP_MESSAGE_NOT_ADMINISTRATOR_CANNOT_INSTALL);
      return reset_failed_command(argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"is-enabled")) return is_enabled_command(argc - 2, argv + 2);
    if (strings_equal_ignore_case(argv[1], L"enable") ||
        strings_equal_ignore_case(argv[1], L"disable") ||
        strings_equal_ignore_case(argv[1], L"mask") ||
        strings_equal_ignore_case(argv[1], L"unmask")) {
      if (! is_admin) return elevate(argc, argv, SVP_MESSAGE_NOT_ADMINISTRATOR_CANNOT_INSTALL);
      const unsigned long start_type =
          strings_equal_ignore_case(argv[1], L"enable") ? SERVICE_AUTO_START :
          strings_equal_ignore_case(argv[1], L"mask") ? SERVICE_DISABLED :
          SERVICE_DEMAND_START;
      return set_service_startup_command(start_type, argc - 2, argv + 2);
    }
    if (strings_equal_ignore_case(argv[1], L"processes")) return service_process_tree(argc - 2, argv + 2);
    if (strings_equal_ignore_case(argv[1], L"remove")) {
      if (! is_admin) {
        return elevate(argc, argv,
                       SVP_MESSAGE_NOT_ADMINISTRATOR_CANNOT_REMOVE);
      }
      return pre_remove_service(argc - 2, argv + 2);
    }
  }

  /* Register messages */
  if (is_admin) create_messages(svp_unquoted_imagepath());

  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<wchar_t *>(kProductName), service_main}, {nullptr, nullptr}};
  if (! StartServiceCtrlDispatcherW(table)) {
    unsigned long error = GetLastError();
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) return usage(1);
    log_event(EVENTLOG_ERROR_TYPE, SVP_EVENT_DISPATCHER_FAILED,
              error_string(error), 0);
    return 100;
  }

  /* And nothing more to do */
  return 0;
}
