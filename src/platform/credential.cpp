#include "credential.h"

#include "win32_handle.h"

#include <windows.h>
#include <dpapi.h>
#include <sddl.h>

namespace {
constexpr wchar_t kCredentialValue[] = L"Credential";
constexpr wchar_t kCredentialAccountValue[] = L"CredentialAccount";

std::wstring key_path(const std::wstring &service) {
  return L"SOFTWARE\\SvcPilot\\Credentials\\" + service;
}

bool credential_security_attributes(
    unique_local_memory<void> *descriptor,
    SECURITY_ATTRIBUTES *attributes, unsigned long *error) {
  if (! ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;KA;;;SY)(A;;KA;;;BA)", SDDL_REVISION_1,
          descriptor->put(), nullptr)) {
    if (error) *error = GetLastError();
    return false;
  }
  *attributes = {sizeof(*attributes), descriptor->get(), FALSE};
  return true;
}

bool open_key(const std::wstring &service, REGSAM access, bool create,
              unique_registry_key *key, unsigned long *error) {
  long result = ERROR_SUCCESS;
  if (create) {
    unique_local_memory<void> descriptor;
    SECURITY_ATTRIBUTES attributes = {};
    if (! credential_security_attributes(&descriptor, &attributes, error)) {
      return false;
    }
    unsigned long disposition = 0;
    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path(service).c_str(), 0,
                             nullptr, 0, access, &attributes, key->put(),
                             &disposition);
  }
  else result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path(service).c_str(), 0,
                              access, key->put());
  if (error) *error = result;
  return result == ERROR_SUCCESS;
}
}

void secure_clear(std::vector<wchar_t> *value) {
  if (! value) return;
  if (! value->empty()) SecureZeroMemory(value->data(), value->size() * sizeof(wchar_t));
  value->clear();
  value->shrink_to_fit();
}

bool store_service_credential(const std::wstring &service,
                              const std::wstring &account,
                              const std::wstring &password,
                              unsigned long *error) {
  DATA_BLOB input = {static_cast<DWORD>((password.size() + 1) * sizeof(wchar_t)),
                     reinterpret_cast<BYTE *>(const_cast<wchar_t *>(password.c_str()))};
  DATA_BLOB entropy = {static_cast<DWORD>((service.size() + account.size()) * sizeof(wchar_t)), nullptr};
  std::wstring binding = service + account;
  entropy.pbData = reinterpret_cast<BYTE *>(binding.data());
  DATA_BLOB output = {};
  if (! CryptProtectData(&input, L"SvcPilot service credential", &entropy,
                         nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE,
                         &output)) {
    if (error) *error = GetLastError();
    return false;
  }
  unique_local_memory<void> encrypted(output.pbData);
  unique_registry_key key;
  if (! open_key(service, KEY_SET_VALUE | WRITE_DAC | READ_CONTROL, true, &key,
                 error)) return false;
  long result = RegSetValueExW(key.get(), kCredentialValue, 0, REG_BINARY,
                               output.pbData, output.cbData);
  if (result == ERROR_SUCCESS) {
    result = RegSetValueExW(key.get(), kCredentialAccountValue, 0, REG_SZ,
        reinterpret_cast<const BYTE *>(account.c_str()),
        static_cast<DWORD>((account.size() + 1) * sizeof(wchar_t)));
  }
  if (error) *error = result;
  return result == ERROR_SUCCESS;
}

bool load_service_credential(const std::wstring &service,
                             const std::wstring &account,
                             std::vector<wchar_t> *password,
                             unsigned long *error) {
  if (! password) return false;
  unique_registry_key key;
  if (! open_key(service, KEY_QUERY_VALUE, false, &key, error)) return false;
  wchar_t stored_account[512] = {};
  DWORD type = 0, bytes = sizeof(stored_account);
  long result = RegQueryValueExW(key.get(), kCredentialAccountValue, nullptr,
      &type, reinterpret_cast<BYTE *>(stored_account), &bytes);
  if (result != ERROR_SUCCESS || type != REG_SZ || account != stored_account) {
    if (error) *error = result == ERROR_SUCCESS ? ERROR_INVALID_ACCOUNT_NAME : result;
    return false;
  }
  bytes = 0;
  result = RegQueryValueExW(key.get(), kCredentialValue, nullptr, &type, nullptr, &bytes);
  if (result != ERROR_SUCCESS || type != REG_BINARY || ! bytes) {
    if (error) *error = result == ERROR_SUCCESS ? ERROR_INVALID_DATA : result;
    return false;
  }
  std::vector<BYTE> encrypted(bytes);
  result = RegQueryValueExW(key.get(), kCredentialValue, nullptr, &type,
                           encrypted.data(), &bytes);
  if (result != ERROR_SUCCESS) { if (error) *error = result; return false; }
  std::wstring binding = service + account;
  DATA_BLOB input = {bytes, encrypted.data()};
  DATA_BLOB entropy = {static_cast<DWORD>(binding.size() * sizeof(wchar_t)),
                       reinterpret_cast<BYTE *>(binding.data())};
  DATA_BLOB output = {};
  if (! CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, 0,
                           &output)) {
    if (error) *error = GetLastError();
    return false;
  }
  unique_local_memory<void> clear(output.pbData);
  if (output.cbData < sizeof(wchar_t) || output.cbData % sizeof(wchar_t)) {
    if (error) *error = ERROR_INVALID_DATA;
    return false;
  }
  auto *characters = static_cast<wchar_t *>(clear.get());
  const size_t count = output.cbData / sizeof(wchar_t);
  if (characters[count - 1] != L'\0') { if (error) *error = ERROR_INVALID_DATA; return false; }
  password->assign(characters, characters + count);
  SecureZeroMemory(characters, output.cbData);
  if (error) *error = ERROR_SUCCESS;
  return true;
}

bool delete_service_credential(const std::wstring &service, unsigned long *error) {
  const long result = RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                                     key_path(service).c_str());
  const long normalized = result == ERROR_FILE_NOT_FOUND ||
                                  result == ERROR_PATH_NOT_FOUND
                              ? ERROR_SUCCESS
                              : result;
  if (error) *error = normalized;
  return normalized == ERROR_SUCCESS;
}

bool service_credential_exists(const std::wstring &service,
                               const std::wstring &account, bool *exists,
                               unsigned long *error) {
  std::vector<wchar_t> password;
  const bool loaded = load_service_credential(service, account, &password, error);
  secure_clear(&password);
  if (exists) *exists = loaded;
  if (! loaded && error && (*error == ERROR_FILE_NOT_FOUND || *error == ERROR_PATH_NOT_FOUND)) {
    *error = ERROR_SUCCESS;
    return true;
  }
  return loaded;
}
