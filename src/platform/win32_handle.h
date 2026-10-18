#ifndef WIN32_HANDLE_H
#define WIN32_HANDLE_H

#include <windows.h>

#include <utility>

template <typename Handle, auto Close>
class unique_win32_resource {
 public:
  unique_win32_resource() noexcept = default;
  explicit unique_win32_resource(Handle handle) noexcept : handle_(handle) {}

  ~unique_win32_resource() noexcept { reset(); }

  unique_win32_resource(const unique_win32_resource &) = delete;
  unique_win32_resource &operator=(const unique_win32_resource &) = delete;

  unique_win32_resource(unique_win32_resource &&other) noexcept
      : handle_(other.release()) {}

  unique_win32_resource &operator=(unique_win32_resource &&other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

  [[nodiscard]] Handle get() const noexcept { return handle_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return handle_ != nullptr;
  }

  [[nodiscard]] Handle release() noexcept {
    return std::exchange(handle_, nullptr);
  }

  void reset(Handle handle = nullptr) noexcept {
    Handle old = std::exchange(handle_, handle);
    if (old != nullptr) Close(old);
  }

  Handle *put() noexcept {
    reset();
    return &handle_;
  }

 private:
  Handle handle_ = nullptr;
};

using unique_service_handle =
    unique_win32_resource<SC_HANDLE, CloseServiceHandle>;
using unique_registry_key = unique_win32_resource<HKEY, RegCloseKey>;
using unique_module = unique_win32_resource<HMODULE, FreeLibrary>;
using unique_event_source =
    unique_win32_resource<HANDLE, DeregisterEventSource>;
using unique_mapped_view =
    unique_win32_resource<void *, UnmapViewOfFile>;

class unique_handle {
 public:
  unique_handle() noexcept = default;
  explicit unique_handle(HANDLE handle) noexcept : handle_(handle) {}
  ~unique_handle() noexcept { reset(); }

  unique_handle(const unique_handle &) = delete;
  unique_handle &operator=(const unique_handle &) = delete;

  unique_handle(unique_handle &&other) noexcept : handle_(other.release()) {}
  unique_handle &operator=(unique_handle &&other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(handle_); }

  [[nodiscard]] HANDLE release() noexcept {
    return std::exchange(handle_, nullptr);
  }

  void reset(HANDLE handle = nullptr) noexcept {
    HANDLE old = std::exchange(handle_, handle);
    if (valid(old)) CloseHandle(old);
  }

  HANDLE *put() noexcept {
    reset();
    return &handle_;
  }

 private:
  static bool valid(HANDLE handle) noexcept {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
  }

  HANDLE handle_ = nullptr;
};

template <typename T>
class unique_local_memory {
 public:
  unique_local_memory() noexcept = default;
  explicit unique_local_memory(T *memory) noexcept : memory_(memory) {}
  ~unique_local_memory() noexcept { reset(); }

  unique_local_memory(const unique_local_memory &) = delete;
  unique_local_memory &operator=(const unique_local_memory &) = delete;

  unique_local_memory(unique_local_memory &&other) noexcept
      : memory_(other.release()) {}
  unique_local_memory &operator=(unique_local_memory &&other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

  [[nodiscard]] T *get() const noexcept { return memory_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return memory_ != nullptr;
  }

  [[nodiscard]] T *release() noexcept {
    return std::exchange(memory_, nullptr);
  }

  void reset(T *memory = nullptr) noexcept {
    T *old = std::exchange(memory_, memory);
    if (old != nullptr) LocalFree(old);
  }

 private:
  T *memory_ = nullptr;
};

#endif
