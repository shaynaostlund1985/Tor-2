/* Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007, The Tor Project, Inc. */
/* See LICENSE for licensing information */
/* $Id$ */

#define MAIN_PRIVATE
#include "or.h"

const char ntmain_c_id[] =
  "$Id$";

#include <tchar.h>
#define GENSRV_SERVICENAME  TEXT("tor")
#define GENSRV_DISPLAYNAME  TEXT("Tor Win32 Service")
#define GENSRV_DESCRIPTION  \
  TEXT("Provides an anonymous Internet communication system")
#define GENSRV_USERACCT TEXT("NT AUTHORITY\\LocalService")

// Cheating: using the pre-defined error codes, tricks Windows into displaying
//           a semi-related human-readable error message if startup fails as
//           opposed to simply scaring people with Error: 0xffffffff
#define NT_SERVICE_ERROR_TORINIT_FAILED ERROR_EXCEPTION_IN_SERVICE

static SERVICE_STATUS service_status;
static SERVICE_STATUS_HANDLE hStatus;

/* XXXX020 This 'backup argv' and 'backup argc' business is an ugly hack. This
 * is a job for arguments, not globals. */
static char **backup_argv;
static int backup_argc;
static char* nt_strerror(uint32_t errnum);

static void nt_service_control(DWORD request);
static void nt_service_body(int argc, char **argv);
static void nt_service_main(void);
static SC_HANDLE nt_service_open_scm(void);
static SC_HANDLE nt_service_open(SC_HANDLE hSCManager);
static int nt_service_start(SC_HANDLE hService);
static int nt_service_stop(SC_HANDLE hService);
static int nt_service_install(int argc, char **argv);
static int nt_service_remove(void);
static int nt_service_cmd_start(void);
static int nt_service_cmd_stop(void);

struct service_fns {
  int loaded;

  BOOL (WINAPI *ChangeServiceConfig2A_fn)(
                             SC_HANDLE hService,
                             DWORD dwInfoLevel,
                             LPVOID lpInfo);

  BOOL (WINAPI *CloseServiceHandle_fn)(
                             SC_HANDLE hSCObject);

  BOOL (WINAPI *ControlService_fn)(
                             SC_HANDLE hService,
                             DWORD dwControl,
                             LPSERVICE_STATUS lpServiceStatus);

  SC_HANDLE (WINAPI *CreateServiceA_fn)(
                             SC_HANDLE hSCManager,
                             LPCTSTR lpServiceName,
                             LPCTSTR lpDisplayName,
                             DWORD dwDesiredAccess,
                             DWORD dwServiceType,
                             DWORD dwStartType,
                             DWORD dwErrorControl,
                             LPCTSTR lpBinaryPathName,
                             LPCTSTR lpLoadOrderGroup,
                             LPDWORD lpdwTagId,
                             LPCTSTR lpDependencies,
                             LPCTSTR lpServiceStartName,
                             LPCTSTR lpPassword);

  BOOL (WINAPI *DeleteService_fn)(
                             SC_HANDLE hService);

  SC_HANDLE (WINAPI *OpenSCManagerA_fn)(
                             LPCTSTR lpMachineName,
                             LPCTSTR lpDatabaseName,
                             DWORD dwDesiredAccess);

  SC_HANDLE (WINAPI *OpenServiceA_fn)(
                             SC_HANDLE hSCManager,
                             LPCTSTR lpServiceName,
                             DWORD dwDesiredAccess);

  BOOL (WINAPI *QueryServiceStatus_fn)(
                             SC_HANDLE hService,
                             LPSERVICE_STATUS lpServiceStatus);

  SERVICE_STATUS_HANDLE (WINAPI *RegisterServiceCtrlHandlerA_fn)(
                             LPCTSTR lpServiceName,
                             LPHANDLER_FUNCTION lpHandlerProc);

  BOOL (WINAPI *SetServiceStatus_fn)(SERVICE_STATUS_HANDLE,
                             LPSERVICE_STATUS);

  BOOL (WINAPI *StartServiceCtrlDispatcherA_fn)(
                             const SERVICE_TABLE_ENTRY* lpServiceTable);

  BOOL (WINAPI *StartServiceA_fn)(
                             SC_HANDLE hService,
                             DWORD dwNumServiceArgs,
                             LPCTSTR* lpServiceArgVectors);

  BOOL (WINAPI *LookupAccountNameA_fn)(
                             LPCTSTR lpSystemName,
                             LPCTSTR lpAccountName,
                             PSID Sid,
                             LPDWORD cbSid,
                             LPTSTR ReferencedDomainName,
                             LPDWORD cchReferencedDomainName,
                             PSID_NAME_USE peUse);
} service_fns = { 0,
                  NULL, NULL, NULL, NULL, NULL, NULL,
                  NULL, NULL, NULL, NULL, NULL, NULL,
                  NULL};

/** Loads functions used by NT services. Returns on success, or prints a
 * complaint to stdout and exits on error. */
static void
nt_service_loadlibrary(void)
{
  HMODULE library = 0;
  void *fn;

  if (service_fns.loaded)
    return;

  /* XXXX Possibly, we should hardcode the location of this DLL. */
  if (!(library = LoadLibrary("advapi32.dll"))) {
    log_err(LD_GENERAL, "Couldn't open advapi32.dll.  Are you trying to use "
            "NT services on Windows 98? That doesn't work.");
    goto err;
  }

#define LOAD(f) STMT_BEGIN                                              \
    if (!(fn = GetProcAddress(library, #f))) {                          \
      log_err(LD_BUG,                                                   \
              "Couldn't find %s in advapi32.dll! We probably got the "  \
              "name wrong.", #f);                                       \
      goto err;                                                         \
    } else {                                                            \
      service_fns.f ## _fn = fn;                                        \
    }                                                                   \
  STMT_END

  LOAD(ChangeServiceConfig2A);
  LOAD(CloseServiceHandle);
  LOAD(ControlService);
  LOAD(CreateServiceA);
  LOAD(DeleteService);
  LOAD(OpenSCManagerA);
  LOAD(OpenServiceA);
  LOAD(QueryServiceStatus);
  LOAD(RegisterServiceCtrlHandlerA);
  LOAD(SetServiceStatus);
  LOAD(StartServiceCtrlDispatcherA);
  LOAD(StartServiceA);
  LOAD(LookupAccountNameA);

  service_fns.loaded = 1;

  return;
 err:
  printf("Unable to load library support for NT services: exiting.\n");
  exit(1);
}

/** If we're compiled to run as an NT service, and the service wants to
 * shut down, then change our current status and return 1.  Else
 * return 0.
 */
int
nt_service_is_stopping(void)
/* XXXX this function would probably _love_ to be inline, in 0.2.0. */
{
  /* If we haven't loaded the function pointers, we can't possibly be an NT
   * service trying to shut down. */
  if (!service_fns.loaded)
    return 0;

  if (service_status.dwCurrentState == SERVICE_STOP_PENDING) {
    service_status.dwWin32ExitCode = 0;
    service_status.dwCurrentState = SERVICE_STOPPED;
    service_fns.SetServiceStatus_fn(hStatus, &service_status);
    return 1;
  } else if (service_status.dwCurrentState == SERVICE_STOPPED) {
    return 1;
  }
  return 0;
}

/** Set the dwCurrentState field for our service to <b>state</b>. */
void
nt_service_set_state(DWORD state)
{
  service_status.dwCurrentState = state;
}

/** Handles service control requests, such as stopping or starting the
 * Tor service. */
static void
nt_service_control(DWORD request)
{
  static struct timeval exit_now;
  exit_now.tv_sec  = 0;
  exit_now.tv_usec = 0;

  nt_service_loadlibrary();

  switch (request) {
    case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
          log_notice(LD_GENERAL,
                     "Got stop/shutdown request; shutting down cleanly.");
          service_status.dwCurrentState = SERVICE_STOP_PENDING;
          event_loopexit(&exit_now);
          return;
  }
  service_fns.SetServiceStatus_fn(hStatus, &service_status);
}

/** Called when the service is started via the system's service control
 * manager. This calls tor_init() and starts the main event loop. If
 * tor_init() fails, the service will be stopped and exit code set to
 * NT_SERVICE_ERROR_TORINIT_FAILED. */
static void
nt_service_body(int argc, char **argv)
{
  int r;
  (void) argc; /* unused */
  (void) argv; /* unused */
  nt_service_loadlibrary();
  service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service_status.dwCurrentState = SERVICE_START_PENDING;
  service_status.dwControlsAccepted =
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  service_status.dwWin32ExitCode = 0;
  service_status.dwServiceSpecificExitCode = 0;
  service_status.dwCheckPoint = 0;
  service_status.dwWaitHint = 1000;
  hStatus = service_fns.RegisterServiceCtrlHandlerA_fn(GENSRV_SERVICENAME,
                                   (LPHANDLER_FUNCTION) nt_service_control);

  if (hStatus == 0) {
    /* Failed to register the service control handler function */
    return;
  }

  r = tor_init(backup_argc, backup_argv);
  if (r) {
    /* Failed to start the Tor service */
    r = NT_SERVICE_ERROR_TORINIT_FAILED;
    service_status.dwCurrentState = SERVICE_STOPPED;
    service_status.dwWin32ExitCode = r;
    service_status.dwServiceSpecificExitCode = r;
    service_fns.SetServiceStatus_fn(hStatus, &service_status);
    return;
  }

  /* Set the service's status to SERVICE_RUNNING and start the main
   * event loop */
  service_status.dwCurrentState = SERVICE_RUNNING;
  service_fns.SetServiceStatus_fn(hStatus, &service_status);
  do_main_loop();
  tor_cleanup();
}

/** Main service entry point. Starts the service control dispatcher and waits
 * until the service status is set to SERVICE_STOPPED. */
static void
nt_service_main(void)
{
  SERVICE_TABLE_ENTRY table[2];
  DWORD result = 0;
  char *errmsg;
  nt_service_loadlibrary();
  table[0].lpServiceName = (char*)GENSRV_SERVICENAME;
  table[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)nt_service_body;
  table[1].lpServiceName = NULL;
  table[1].lpServiceProc = NULL;

  if (!service_fns.StartServiceCtrlDispatcherA_fn(table)) {
    result = GetLastError();
    errmsg = nt_strerror(result);
    printf("Service error %d : %s\n", (int) result, errmsg);
    LocalFree(errmsg);
    if (result == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      if (tor_init(backup_argc, backup_argv) < 0)
        return;
      switch (get_options()->command) {
      case CMD_RUN_TOR:
        do_main_loop();
        break;
      case CMD_LIST_FINGERPRINT:
        do_list_fingerprint();
        break;
      case CMD_HASH_PASSWORD:
        do_hash_password();
        break;
      case CMD_VERIFY_CONFIG:
        printf("Configuration was valid\n");
        break;
      case CMD_RUN_UNITTESTS:
      default:
        log_err(LD_CONFIG, "Illegal command number %d: internal error.",
                get_options()->command);
      }
      tor_cleanup();
    }
  }
}

/** Return a handle to the service control manager on success, or NULL on
 * failure. */
static SC_HANDLE
nt_service_open_scm(void)
{
  SC_HANDLE hSCManager;
  char *errmsg = NULL;

  nt_service_loadlibrary();
  if ((hSCManager = service_fns.OpenSCManagerA_fn(
                            NULL, NULL, SC_MANAGER_CREATE_SERVICE)) == NULL) {
    errmsg = nt_strerror(GetLastError());
    printf("OpenSCManager() failed : %s\n", errmsg);
    LocalFree(errmsg);
  }
  return hSCManager;
}

/** Open a handle to the Tor service using <b>hSCManager</b>. Return NULL
 * on failure. */
static SC_HANDLE
nt_service_open(SC_HANDLE hSCManager)
{
  SC_HANDLE hService;
  char *errmsg = NULL;
  nt_service_loadlibrary();
  if ((hService = service_fns.OpenServiceA_fn(hSCManager, GENSRV_SERVICENAME,
                              SERVICE_ALL_ACCESS)) == NULL) {
    errmsg = nt_strerror(GetLastError());
    printf("OpenService() failed : %s\n", errmsg);
    LocalFree(errmsg);
  }
  return hService;
}

/** Start the Tor service. Return 0 if the service is started or was
 * previously running. Return -1 on error. */
static int
nt_service_start(SC_HANDLE hService)
{
  char *errmsg = NULL;

  nt_service_loadlibrary();

  service_fns.QueryServiceStatus_fn(hService, &service_status);
  if (service_status.dwCurrentState == SERVICE_RUNNING) {
    printf("Service is already running\n");
    return 0;
  }

  if (service_fns.StartServiceA_fn(hService, 0, NULL)) {
    /* Loop until the service has finished attempting to start */
    while (service_fns.QueryServiceStatus_fn(hService, &service_status) &&
           (service_status.dwCurrentState == SERVICE_START_PENDING)) {
      Sleep(500);
    }

    /* Check if it started successfully or not */
    if (service_status.dwCurrentState == SERVICE_RUNNING) {
      printf("Service started successfully\n");
      return 0;
    } else {
      errmsg = nt_strerror(service_status.dwWin32ExitCode);
      printf("Service failed to start : %s\n", errmsg);
      LocalFree(errmsg);
    }
  } else {
    errmsg = nt_strerror(GetLastError());
    printf("StartService() failed : %s\n", errmsg);
    LocalFree(errmsg);
  }
  return -1;
}

/** Stop the Tor service. Return 0 if the service is stopped or was not
 * previously running. Return -1 on error. */
static int
nt_service_stop(SC_HANDLE hService)
{
/** Wait at most 10 seconds for the service to stop. */
#define MAX_SERVICE_WAIT_TIME 10
  int wait_time;
  char *errmsg = NULL;
  nt_service_loadlibrary();

  service_fns.QueryServiceStatus_fn(hService, &service_status);
  if (service_status.dwCurrentState == SERVICE_STOPPED) {
    printf("Service is already stopped\n");
    return 0;
  }

  if (service_fns.ControlService_fn(hService, SERVICE_CONTROL_STOP,
                                    &service_status)) {
    wait_time = 0;
    while (service_fns.QueryServiceStatus_fn(hService, &service_status) &&
           (service_status.dwCurrentState != SERVICE_STOPPED) &&
           (wait_time < MAX_SERVICE_WAIT_TIME)) {
      Sleep(1000);
      wait_time++;
    }
    if (service_status.dwCurrentState == SERVICE_STOPPED) {
      printf("Service stopped successfully\n");
      return 0;
    } else if (wait_time == MAX_SERVICE_WAIT_TIME) {
      printf("Service did not stop within %d seconds.\n", wait_time);
    } else {
      errmsg = nt_strerror(GetLastError());
      printf("QueryServiceStatus() failed : %s\n",errmsg);
      LocalFree(errmsg);
    }
  } else {
    errmsg = nt_strerror(GetLastError());
    printf("ControlService() failed : %s\n", errmsg);
    LocalFree(errmsg);
  }
  return -1;
}

/** Build a formatted command line used for the NT service. Return a
 * pointer to the formatted string on success, or NULL on failure.  Set
 * *<b>using_default_torrc</b> to true if we're going to use the default
 * location to torrc, or 1 if an option was specified on the command line.
 */
static char *
nt_service_command_line(int *using_default_torrc)
{
  TCHAR tor_exe[MAX_PATH+1];
  char *command, *options=NULL;
  smartlist_t *sl;
  int i, cmdlen;
  *using_default_torrc = 1;

  /* Get the location of tor.exe */
  if (0 == GetModuleFileName(NULL, tor_exe, MAX_PATH))
    return NULL;

  /* Get the service arguments */
  sl = smartlist_create();
  for (i = 1; i < backup_argc; ++i) {
    if (!strcmp(backup_argv[i], "--options") ||
        !strcmp(backup_argv[i], "-options")) {
      while (++i < backup_argc) {
        if (!strcmp(backup_argv[i], "-f"))
          *using_default_torrc = 0;
        smartlist_add(sl, backup_argv[i]);
      }
    }
  }
  if (smartlist_len(sl))
    options = smartlist_join_strings(sl,"\" \"",0,NULL);
  smartlist_free(sl);

  /* Allocate a string for the NT service command line */
  cmdlen = strlen(tor_exe) + (options?strlen(options):0) + 32;
  command = tor_malloc(cmdlen);

  /* Format the service command */
  if (options) {
    if (tor_snprintf(command, cmdlen, "\"%s\" --nt-service \"%s\"",
                     tor_exe, options)<0) {
      tor_free(command); /* sets command to NULL. */
    }
  } else { /* ! options */
    if (tor_snprintf(command, cmdlen, "\"%s\" --nt-service", tor_exe)<0) {
      tor_free(command); /* sets command to NULL. */
    }
  }

  tor_free(options);
  return command;
}

/** Creates a Tor NT service, set to start on boot. The service will be
 * started if installation succeeds. Returns 0 on success, or -1 on
 * failure. */
static int
nt_service_install(int argc, char **argv)
{
  /* Notes about developing NT services:
   *
   * 1. Don't count on your CWD. If an absolute path is not given, the
   *    fopen() function goes wrong.
   * 2. The parameters given to the nt_service_body() function differ
   *    from those given to main() function.
   */

  SC_HANDLE hSCManager = NULL;
  SC_HANDLE hService = NULL;
  SERVICE_DESCRIPTION sdBuff;
  char *command;
  char *errmsg;
  const char *user_acct = GENSRV_USERACCT;
  const char *password = "";
  int i;
  OSVERSIONINFOEX info;
  SID_NAME_USE sidUse;
  DWORD sidLen = 0, domainLen = 0;
  int is_win2k_or_worse = 0;
  int using_default_torrc = 0;

  nt_service_loadlibrary();

  /* Open the service control manager so we can create a new service */
  if ((hSCManager = nt_service_open_scm()) == NULL)
    return -1;
  /* Build the command line used for the service */
  if ((command = nt_service_command_line(&using_default_torrc)) == NULL) {
    printf("Unable to build service command line.\n");
    service_fns.CloseServiceHandle_fn(hSCManager);
    return -1;
  }

  for (i=1; i < argc; ++i) {
    if (!strcmp(argv[i], "--user") && i+1<argc) {
      user_acct = argv[i+1];
      ++i;
    }
    if (!strcmp(argv[i], "--password") && i+1<argc) {
      password = argv[i+1];
      ++i;
    }
  }

  /* Compute our version and see whether we're running win2k or earlier. */
  memset(&info, 0, sizeof(info));
  info.dwOSVersionInfoSize = sizeof(info);
  if (! GetVersionEx((LPOSVERSIONINFO)&info)) {
    printf("Call to GetVersionEx failed.\n");
    is_win2k_or_worse = 1;
  } else {
    if (info.dwMajorVersion < 5 ||
        (info.dwMajorVersion == 5 && info.dwMinorVersion == 0))
      is_win2k_or_worse = 1;
  }

  if (user_acct == GENSRV_USERACCT) {
    if (is_win2k_or_worse) {
      /* On Win2k, there is no LocalService account, so we actually need to
       * fall back on NULL (the system account). */
      printf("Running on Win2K or earlier, so the LocalService account "
             "doesn't exist.  Falling back to SYSTEM account.\n");
      user_acct = NULL;
    } else {
      /* Genericity is apparently _so_ last year in Redmond, where some
       * accounts are accounts that you can look up, and some accounts
       * are magic and undetectable via the security subsystem. See
       * http://msdn2.microsoft.com/en-us/library/ms684188.aspx
       */
      printf("Running on a Post-Win2K OS, so we'll assume that the "
             "LocalService account exists.\n");
    }
  } else if (0 && service_fns.LookupAccountNameA_fn(NULL, // On this system
                            user_acct,
                            NULL, &sidLen, // Don't care about the SID
                            NULL, &domainLen, // Don't care about the domain
                            &sidUse) == 0) {
    /* XXXX For some reason, the above test segfaults. Fix that. */
    printf("User \"%s\" doesn't seem to exist.\n", user_acct);
    return -1;
  } else {
    printf("Will try to install service as user \"%s\".\n", user_acct);
  }
  /* XXXX This warning could be better about explaining how to resolve the
   * situation. */
  if (using_default_torrc)
    printf("IMPORTANT NOTE:\n"
        "    The Tor service will run under the account \"%s\".  This means\n"
        "    that Tor will look for its configuration file under that\n"
        "    account's Application Data directory, which is probably not\n"
        "    the same as yours.\n", user_acct?user_acct:"<local system>");

  /* Create the Tor service, set to auto-start on boot */
  if ((hService = service_fns.CreateServiceA_fn(hSCManager, GENSRV_SERVICENAME,
                                GENSRV_DISPLAYNAME,
                                SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                SERVICE_AUTO_START, SERVICE_ERROR_IGNORE,
                                command, NULL, NULL, NULL,
                                user_acct, password)) == NULL) {
    errmsg = nt_strerror(GetLastError());
    printf("CreateService() failed : %s\n", errmsg);
    service_fns.CloseServiceHandle_fn(hSCManager);
    LocalFree(errmsg);
    tor_free(command);
    return -1;
  }
  printf("Done with CreateService.\n");

  /* Set the service's description */
  sdBuff.lpDescription = (char*)GENSRV_DESCRIPTION;
  service_fns.ChangeServiceConfig2A_fn(hService, SERVICE_CONFIG_DESCRIPTION,
                                       &sdBuff);
  printf("Service installed successfully\n");

  /* Start the service initially */
  nt_service_start(hService);

  service_fns.CloseServiceHandle_fn(hService);
  service_fns.CloseServiceHandle_fn(hSCManager);
  tor_free(command);

  return 0;
}

/** Removes the Tor NT service. Returns 0 if the service was successfully
 * removed, or -1 on error. */
static int
nt_service_remove(void)
{
  SC_HANDLE hSCManager = NULL;
  SC_HANDLE hService = NULL;
  char *errmsg;

  nt_service_loadlibrary();
  if ((hSCManager = nt_service_open_scm()) == NULL)
    return -1;
  if ((hService = nt_service_open(hSCManager)) == NULL) {
    service_fns.CloseServiceHandle_fn(hSCManager);
    return -1;
  }

  nt_service_stop(hService);
  if (service_fns.DeleteService_fn(hService) == FALSE) {
    errmsg = nt_strerror(GetLastError());
    printf("DeleteService() failed : %s\n", errmsg);
    LocalFree(errmsg);
    service_fns.CloseServiceHandle_fn(hService);
    service_fns.CloseServiceHandle_fn(hSCManager);
    return -1;
  }

  service_fns.CloseServiceHandle_fn(hService);
  service_fns.CloseServiceHandle_fn(hSCManager);
  printf("Service removed successfully\n");

  return 0;
}

/** Starts the Tor service. Returns 0 on success, or -1 on error. */
static int
nt_service_cmd_start(void)
{
  SC_HANDLE hSCManager;
  SC_HANDLE hService;
  int start;

  if ((hSCManager = nt_service_open_scm()) == NULL)
    return -1;
  if ((hService = nt_service_open(hSCManager)) == NULL) {
    service_fns.CloseServiceHandle_fn(hSCManager);
    return -1;
  }

  start = nt_service_start(hService);
  service_fns.CloseServiceHandle_fn(hService);
  service_fns.CloseServiceHandle_fn(hSCManager);

  return start;
}

/** Stops the Tor service. Returns 0 on success, or -1 on error. */
static int
nt_service_cmd_stop(void)
{
  SC_HANDLE hSCManager;
  SC_HANDLE hService;
  int stop;

  if ((hSCManager = nt_service_open_scm()) == NULL)
    return -1;
  if ((hService = nt_service_open(hSCManager)) == NULL) {
    service_fns.CloseServiceHandle_fn(hSCManager);
    return -1;
  }

  stop = nt_service_stop(hService);
  service_fns.CloseServiceHandle_fn(hService);
  service_fns.CloseServiceHandle_fn(hSCManager);

  return stop;
}

/** Given a Win32 error code, this attempts to make Windows
 * return a human-readable error message. The char* returned
 * is allocated by Windows, but should be freed with LocalFree()
 * when finished with it. */
static char*
nt_strerror(uint32_t errnum)
{
   char *msgbuf;
   FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                 NULL, errnum, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 (LPSTR)&msgbuf, 0, NULL);
   return msgbuf;
}

int
nt_service_parse_options(int argc, char **argv, int *should_exit)
{
  backup_argv = argv;
  backup_argc = argc;
  *should_exit = 0;

  if ((argc >= 3) &&
      (!strcmp(argv[1], "-service") || !strcmp(argv[1], "--service"))) {
    nt_service_loadlibrary();
    if (!strcmp(argv[2], "install"))
      return nt_service_install(argc, argv);
    if (!strcmp(argv[2], "remove"))
      return nt_service_remove();
    if (!strcmp(argv[2], "start"))
      return nt_service_cmd_start();
    if (!strcmp(argv[2], "stop"))
      return nt_service_cmd_stop();
    printf("Unrecognized service command '%s'\n", argv[2]);
    *should_exit = 1;
    return 1;
  }
  if (argc >= 2) {
    if (!strcmp(argv[1], "-nt-service") || !strcmp(argv[1], "--nt-service")) {
      nt_service_loadlibrary();
      nt_service_main();
      *should_exit = 1;
      return 0;
    }
    // These values have been deprecated since 0.1.1.2-alpha; we've warned
    // about them since 0.1.2.7-alpha.
    if (!strcmp(argv[1], "-install") || !strcmp(argv[1], "--install")) {
      nt_service_loadlibrary();
      fprintf(stderr,
            "The %s option is deprecated; use \"--service install\" instead.",
            argv[1]);
      *should_exit = 1;
      return nt_service_install(argc, argv);
    }
    if (!strcmp(argv[1], "-remove") || !strcmp(argv[1], "--remove")) {
      nt_service_loadlibrary();
      fprintf(stderr,
            "The %s option is deprecated; use \"--service remove\" instead.",
            argv[1]);
      *should_exit = 1;
      return nt_service_remove();
    }
  }
  *should_exit = 0;
  return 0;
}
