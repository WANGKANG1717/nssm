#ifndef NSSM_H
#define NSSM_H

/*
  MSDN says, basically, that the maximum length of a path is 260 characters,
  which is represented by the constant MAX_PATH.  Except when it isn't.

  The maximum length of a directory path is MAX_PATH - 12 because it must be
  possible to create a file in 8.3 format under any valid directory.

  Unicode versions of filesystem API functions accept paths up to 32767
  characters if the first four (wide) characters are L"\\?\" and each component
  of the path, separated by L"\", does not exceed the value of
  lpMaximumComponentLength returned by GetVolumeInformation(), which is
  probably 255.  But might not be.

  Relative paths are always limited to MAX_PATH because the L"\\?\" prefix
  is not valid for a relative path.

  Note that we don't care about the last two paragraphs because we're only
  concerned with allocating buffers big enough to store valid paths.  If the
  user tries to store invalid paths they will fit in the buffers but the
  application will fail.  The reason for the failure will end up in the
  event log and the user will realise the mistake.

  So that's that cleared up, then.
*/
#include "windows_limits.h"

#include <windows.h>
#include <tchar.h>
#ifndef NSSM_COMPILE_RC
#include <fcntl.h>
#include <io.h>
#include <shlwapi.h>
#include <stdarg.h>
#include <stdio.h>
static_assert(sizeof(TCHAR) == sizeof(wchar_t));
#endif

void nssm_exit(int);
int str_equiv(const TCHAR *, const TCHAR *);
int usage(int);
const TCHAR *nssm_unquoted_imagepath();
const TCHAR *nssm_imagepath();

#define NSSM_ARCHITECTURE _T("64-bit")
#ifdef _DEBUG
#define NSSM_DEBUG _T(" debug")
#else
#define NSSM_DEBUG _T("")
#endif
#define NSSM_CONFIGURATION NSSM_ARCHITECTURE NSSM_DEBUG
#include "version.h"

#endif
