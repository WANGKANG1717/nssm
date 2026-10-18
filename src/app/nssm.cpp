#include "nssm.h"
#include "console.h"
#include "event.h"
#include "messages.h"
#include "output.h"
#include "registry.h"
#include "security.h"
#include "service.h"
#include "service_commands.h"
#include "status.h"
#include "config.h"
#include "utf8.h"

static TCHAR unquoted_imagepath[PATH_LENGTH];
static TCHAR imagepath[PATH_LENGTH];

void nssm_exit(int status) {
  unsetup_utf8();
  exit(status);
}

/* Are two strings case-insensitively equivalent? */
int str_equiv(const TCHAR *a, const TCHAR *b) {
  size_t len = _tcslen(a);
  if (_tcslen(b) != len) return 0;
  if (_tcsnicmp(a, b, len)) return 0;
  return 1;
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
    else if (str_equiv(s, _T("v"))) return true;
  }
  if (str_equiv(s, _T("version"))) return true;
  return false;
}

/* How to use me correctly */
int usage(int ret) {
  print_message(stderr, NSSM_MESSAGE_USAGE, NSSM_VERSION, NSSM_CONFIGURATION, NSSM_DATE);
  return(ret);
}

static int elevate(int argc, TCHAR **argv, unsigned long message) {
  (void) argc;
  print_message(stderr, message);

  SHELLEXECUTEINFO sei;
  ZeroMemory(&sei, sizeof(sei));
  sei.cbSize = sizeof(sei);
  sei.lpVerb = _T("runas");
  sei.lpFile = (TCHAR *) nssm_imagepath();

  TCHAR *args = (TCHAR *) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, EXE_LENGTH * sizeof(TCHAR));
  if (! args) {
    print_message(stderr, NSSM_MESSAGE_OUT_OF_MEMORY, _T("GetCommandLine()"), _T("elevate()"));
    return 111;
  }

  /* Get command line, which includes the path to NSSM, and skip that part. */
  _sntprintf_s(args, EXE_LENGTH, _TRUNCATE, _T("%s"), GetCommandLine());
  size_t s = _tcslen(argv[0]) + 1;
  if (args[0] == _T('"')) s += 2;
  while (isspace(args[s])) s++;

  sei.lpParameters = args + s;
  sei.nShow = SW_SHOW;

  unsigned long exitcode = 0;
  if (! ShellExecuteEx(&sei)) exitcode = 100;

  HeapFree(GetProcessHeap(), 0, (void *) args);
  return exitcode;
}

const TCHAR *nssm_unquoted_imagepath() {
  return unquoted_imagepath;
}

const TCHAR *nssm_imagepath() {
  return imagepath;
}

int _tmain(int argc, TCHAR **argv) {
  if (console_attached()) setup_utf8();

  const bool is_admin = is_process_administrator();

  /* Remember our path for later. */
  GetModuleFileName(0, unquoted_imagepath, _countof(unquoted_imagepath));
  GetModuleFileName(0, imagepath, _countof(imagepath));
  PathQuoteSpaces(imagepath);

  /* Elevate */
  if (argc > 1) {
    /*
      Valid commands are:
      validate, plan, apply, export, start, stop, restart, pause, continue, remove, status,
      statuscode, rotate, list, processes, version
    */
    if (is_version(argv[1])) {
      _tprintf(_T("%s %s %s %s\n"), NSSM, NSSM_VERSION, NSSM_CONFIGURATION, NSSM_DATE);
      nssm_exit(0);
    }
    if (str_equiv(argv[1], _T("validate"))) {
      nssm_exit(validate_service_config_command(argc - 2, argv + 2));
    }
    if (str_equiv(argv[1], _T("plan"))) {
      nssm_exit(plan_service_config_command(argc - 2, argv + 2));
    }
    if (str_equiv(argv[1], _T("export"))) {
      nssm_exit(export_service_config_command(argc - 2, argv + 2));
    }
    if (str_equiv(argv[1], _T("apply"))) {
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
      nssm_exit(apply_service_config_command(argc - 2, argv + 2));
    }
    if (str_equiv(argv[1], _T("start"))) nssm_exit(control_service(NSSM_SERVICE_CONTROL_START, argc - 2, argv + 2));
    if (str_equiv(argv[1], _T("stop"))) nssm_exit(control_service(SERVICE_CONTROL_STOP, argc - 2, argv + 2));
    if (str_equiv(argv[1], _T("restart"))) {
      int ret = control_service(SERVICE_CONTROL_STOP, argc - 2, argv + 2);
      if (ret) nssm_exit(ret);
      nssm_exit(control_service(NSSM_SERVICE_CONTROL_START, argc - 2, argv + 2));
    }
    if (str_equiv(argv[1], _T("pause"))) nssm_exit(control_service(SERVICE_CONTROL_PAUSE, argc - 2, argv + 2));
    if (str_equiv(argv[1], _T("continue"))) nssm_exit(control_service(SERVICE_CONTROL_CONTINUE, argc - 2, argv + 2));
    if (str_equiv(argv[1], _T("status"))) {
      cli_arguments_t arguments = parse_cli_arguments(argc - 2, argv + 2);
      if (arguments.verbose || arguments.json) {
        nssm_exit(inspect_service_status_command(argc - 2, argv + 2));
      }
      nssm_exit(control_service(SERVICE_CONTROL_INTERROGATE, argc - 2, argv + 2));
    }
    if (str_equiv(argv[1], _T("statuscode"))) nssm_exit(control_service(SERVICE_CONTROL_INTERROGATE, argc - 2, argv + 2, true));
    if (str_equiv(argv[1], _T("rotate"))) nssm_exit(control_service(NSSM_SERVICE_CONTROL_ROTATE, argc - 2, argv + 2));
    if (str_equiv(argv[1], _T("list"))) nssm_exit(list_nssm_services(argc - 2, argv + 2));
    if (str_equiv(argv[1], _T("processes"))) nssm_exit(service_process_tree(argc - 2, argv + 2));
    if (str_equiv(argv[1], _T("remove"))) {
      if (! is_admin) nssm_exit(elevate(argc, argv, NSSM_MESSAGE_NOT_ADMINISTRATOR_CANNOT_REMOVE));
      nssm_exit(pre_remove_service(argc - 2, argv + 2));
    }
  }

  /* Register messages */
  if (is_admin) create_messages(nssm_unquoted_imagepath());

  SERVICE_TABLE_ENTRY table[] = { { NSSM, service_main }, { 0, 0 } };
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
