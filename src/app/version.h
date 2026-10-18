#pragma once

#include <tchar.h>

#define NSSM_VERSION _T("3.1.3")
#define NSSM_VERSIONINFO 3,1,3,0
#ifndef NSSM_BUILD_LABEL
#define NSSM_BUILD_LABEL "release"
#endif
#define NSSM_DATE _T(NSSM_BUILD_LABEL)
#define NSSM_ARCHITECTURE _T("64-bit")
#ifdef _DEBUG
#define NSSM_DEBUG _T(" debug")
#else
#define NSSM_DEBUG _T("")
#endif
#define NSSM_CONFIGURATION NSSM_ARCHITECTURE NSSM_DEBUG
#define NSSM_FILEFLAGS 0L
#define NSSM_COPYRIGHT _T("Public Domain; Author fightroad")

/* 可配置的元数据（供 nssm.rc 的 VERSIONINFO 使用） */
#define NSSM_COMPANY _T("WANGKANG1717")
#define NSSM_FILE_DESCRIPTION _T("稳定可靠的服务管理器")
#define NSSM_COMMENTS _T("https://github.com/WANGKANG1717/nssm")
#define NSSM_ORIGINAL_FILENAME _T("nssm.exe")
#define NSSM_PRODUCT_NAME _T("NSSM")

