#include "nssm.h"
#include "child_process.h"
#include "config_commands.h"
#include "console.h"
#include "event.h"
#include "messages.h"
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

void nssm_exit(int status) {
  exit(status);
}

/* User requested us to print our version. */
static bool is_version(const TCHAR *s) {
  if (! s || ! *s) return false;
  /* /version */
  if (*s == '/') s++;
  else if (*s == '-') {
    /* -v, -V, -version, --version */
    s++;
    if (*s == '-') s++;
    else if (strings_equal_ignore_case(s, _T("v"))) return true;
  }
  if (strings_equal_ignore_case(s, _T("version"))) return true;
  return false;
}

static int elevate(int argc, TCHAR **argv, unsigned long message) {
  print_message(stderr, message);

  std::vector<std::wstring> arguments;
  arguments.reserve(argc > 1 ? static_cast<size_t>(argc - 1) : 0);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  std::wstring parameters = join_windows_arguments(arguments);

  SHELLEXECUTEINFO sei = {};
  sei.cbSize = sizeof(sei);
  sei.lpVerb = _T("runas");
  sei.lpFile = nssm_unquoted_imagepath();
  sei.lpParameters = parameters.c_str();
  sei.nShow = SW_SHOW;

  return ShellExecuteEx(&sei) ? 0 : 100;
}

const TCHAR *nssm_unquoted_imagepath() {
  return application_paths().unquoted.c_str();
}

const TCHAR *nssm_imagepath() {
  return application_paths().quoted.c_str();
}

int _tmain(int argc, TCHAR **argv) {
  if (console_attached()) setup_utf8();

  /* Remember our path for later. */
  const application_paths_t &paths = application_paths();
  if (paths.error != ERROR_SUCCESS) {
    fwprintf(stderr, L"Unable to resolve the NSSM executable path: %ls\n",
             error_string(paths.error));
    nssm_exit(100);
  }

  const bool is_admin = is_process_administrator();

  /* Elevate */
  if (argc > 1) {
    /*
      Valid commands are:
      validate, plan, apply, export, start, stop, restart, pause, continue, remove, status,
      statuscode, rotate, list, processes, version
    */
    if (is_version(argv[1])) {
      _tprintf(_T("%s %s %s %s\n"), kNssmServiceName, NSSM_VERSION,
               NSSM_CONFIGURATION, NSSM_DATE);
      nssm_exit(0);
    }
    if (strings_equal_ignore_case(argv[1], _T("validate"))) {
      nssm_exit(validate_service_config_command(argc - 2, argv + 2));
    }
    if (strings_equal_ignore_case(argv[1], _T("plan"))) {
      nssm_exit(plan_service_config_command(argc - 2, argv + 2,
                                            nssm_imagepath()));
    }
    if (strings_equal_ignore_case(argv[1], _T("export"))) {
      nssm_exit(export_service_config_command(argc - 2, argv + 2));
    }
    if (strings_equal_ignore_case(argv[1], _T("apply"))) {
      if (! is_admin) {
        cli_arguments_t arguments = parse_cli_arguments(argc - 2, argv + 2);
        if (arguments.json) {
          print_cli_error(true, L"permission",
                          L"Administrator access is required to apply a service configuration");
          nssm_exit(static_cast<int>(cli_exit_code::permission));
        }
        nssm_exit(elevate(argc, argv, NSSM_MESSAGE_NOT_ADMINISTRATOR_CANNOT_INSTALL));
      }
      create_messages(nssm_unquoted_imagepath());
      nssm_exit(apply_service_config_command(argc - 2, argv + 2,
                                             nssm_imagepath()));
    }
    if (strings_equal_ignore_case(argv[1], _T("start"))) nssm_exit(control_service(kServiceControlStart, argc - 2, argv + 2));
    if (strings_equal_ignore_case(argv[1], _T("stop"))) nssm_exit(control_service(SERVICE_CONTROL_STOP, argc - 2, argv + 2));
    if (strings_equal_ignore_case(argv[1], _T("restart"))) {
      int ret = control_service(SERVICE_CONTROL_STOP, argc - 2, argv + 2);
      if (ret) nssm_exit(ret);
      nssm_exit(control_service(kServiceControlStart, argc - 2, argv + 2));
    }
    if (strings_equal_ignore_case(argv[1], _T("pause"))) nssm_exit(control_service(SERVICE_CONTROL_PAUSE, argc - 2, argv + 2));
    if (strings_equal_ignore_case(argv[1], _T("continue"))) nssm_exit(control_service(SERVICE_CONTROL_CONTINUE, argc - 2, argv + 2));
    if (strings_equal_ignore_case(argv[1], _T("status"))) {
      cli_arguments_t arguments = parse_cli_arguments(argc - 2, argv + 2);
      if (arguments.verbose || arguments.json) {
        nssm_exit(inspect_service_status_command(argc - 2, argv + 2));
      }
      nssm_exit(control_service(SERVICE_CONTROL_INTERROGATE, argc - 2, argv + 2));
    }
    if (strings_equal_ignore_case(argv[1], _T("statuscode"))) nssm_exit(control_service(SERVICE_CONTROL_INTERROGATE, argc - 2, argv + 2, true));
    if (strings_equal_ignore_case(argv[1], _T("rotate"))) nssm_exit(control_service(kServiceControlRotate, argc - 2, argv + 2));
    if (strings_equal_ignore_case(argv[1], _T("list"))) nssm_exit(list_nssm_services(argc - 2, argv + 2));
    if (strings_equal_ignore_case(argv[1], _T("processes"))) nssm_exit(service_process_tree(argc - 2, argv + 2));
    if (strings_equal_ignore_case(argv[1], _T("remove"))) {
      if (! is_admin) nssm_exit(elevate(argc, argv, NSSM_MESSAGE_NOT_ADMINISTRATOR_CANNOT_REMOVE));
      nssm_exit(pre_remove_service(argc - 2, argv + 2));
    }
  }

  /* Register messages */
  if (is_admin) create_messages(nssm_unquoted_imagepath());

  SERVICE_TABLE_ENTRY table[] = {
      {const_cast<wchar_t *>(kNssmServiceName), service_main}, {nullptr, nullptr}};
  if (! StartServiceCtrlDispatcher(table)) {
    unsigned long error = GetLastError();
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) nssm_exit(usage(1));
    log_event(EVENTLOG_ERROR_TYPE, NSSM_EVENT_DISPATCHER_FAILED,
              error_string(error), 0);
    nssm_exit(100);
  }

  /* And nothing more to do */
  nssm_exit(0);
  return 0;
}
