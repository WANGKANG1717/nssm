#ifndef SVP_SERVICE_CONSTANTS_H
#define SVP_SERVICE_CONSTANTS_H

inline constexpr wchar_t kProductName[] = L"SvcPilot";

inline constexpr unsigned long kResetThrottleRestartMs = 1500;
inline constexpr unsigned long kKillConsoleGracePeriodMs = 1500;
inline constexpr unsigned long kKillWindowGracePeriodMs = 1500;
inline constexpr unsigned long kKillThreadsGracePeriodMs = 1500;
inline constexpr unsigned long kServiceWaitHintMarginMs = 2000;
inline constexpr unsigned long kServiceStatusDeadlineMs = 20000;
inline constexpr unsigned long kServiceControlStart = 0;
inline constexpr unsigned long kServiceControlRotate = 128;
inline constexpr unsigned long kServiceControlReload = 129;
inline constexpr unsigned long kLoggerCleanupDeadlineMs = 1500;

#endif
