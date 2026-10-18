#ifndef NSSM_SECURITY_H
#define NSSM_SECURITY_H

#include <windows.h>

#include <string>

class debug_privilege_guard {
 public:
  debug_privilege_guard() noexcept;
  ~debug_privilege_guard() noexcept;

  debug_privilege_guard(const debug_privilege_guard &) = delete;
  debug_privilege_guard &operator=(const debug_privilege_guard &) = delete;

  [[nodiscard]] explicit operator bool() const noexcept { return enabled_; }

 private:
  HANDLE token_ = nullptr;
  TOKEN_PRIVILEGES previous_ = {};
  bool impersonated_ = false;
  bool adjusted_ = false;
  bool enabled_ = false;
};

bool check_secure_path(const std::wstring &, bool, std::wstring *);
bool check_secure_file_handle(HANDLE, std::wstring *);
bool is_process_administrator();

#endif
