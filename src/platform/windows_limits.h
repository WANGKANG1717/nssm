#ifndef NSSM_PLATFORM_WINDOWS_LIMITS_H
#define NSSM_PLATFORM_WINDOWS_LIMITS_H

#include <cstddef>

inline constexpr std::size_t PATH_LENGTH = 32767;
inline constexpr std::size_t DIR_LENGTH = PATH_LENGTH - 12;
inline constexpr std::size_t VALUE_LENGTH = 16383;
inline constexpr std::size_t SERVICE_NAME_LENGTH = 257;

#endif
