#include "security.h"

#include <aclapi.h>
#include <windows.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr ACCESS_MASK kModificationRights =
    FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
    FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC |
    WRITE_OWNER;
constexpr ACCESS_MASK kAncestorReplacementRights =
    FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;

std::wstring parent_path(const std::wstring &path) {
  const auto separator_at = [&path](size_t index) {
    return index < path.size() &&
           (path[index] == L'\\' || path[index] == L'/');
  };
  size_t separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) return L"";
  if (separator == 2 && path.size() >= 3 && path[1] == L':') {
    return path.substr(0, 3);
  }
  if (separator_at(0) && separator_at(1)) {
    const size_t server_end = path.find_first_of(L"\\/", 2);
    if (server_end == std::wstring::npos) return path;
    const size_t share_end = path.find_first_of(L"\\/", server_end + 1);
    if (share_end == std::wstring::npos) return path;
    if (separator == share_end) return path.substr(0, share_end);
  }
  if (! separator) return path.substr(0, 1);
  return path.substr(0, separator);
}

bool make_well_known_sid(WELL_KNOWN_SID_TYPE type,
                         std::vector<unsigned char> *storage) {
  unsigned long size = SECURITY_MAX_SID_SIZE;
  storage->resize(size);
  if (! CreateWellKnownSid(type, nullptr, storage->data(), &size)) return false;
  storage->resize(size);
  return true;
}

bool lookup_sid(const wchar_t *name, std::vector<unsigned char> *storage) {
  unsigned long sid_size = 0;
  unsigned long domain_size = 0;
  SID_NAME_USE use = SidTypeUnknown;
  LookupAccountNameW(nullptr, name, nullptr, &sid_size, nullptr, &domain_size,
                     &use);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || ! sid_size) return false;
  storage->resize(sid_size);
  std::vector<wchar_t> domain(domain_size ? domain_size : 1);
  return LookupAccountNameW(nullptr, name, storage->data(), &sid_size,
                            domain.data(), &domain_size, &use) != FALSE;
}

bool trusted_principal(PSID sid) {
  if (! sid || ! IsValidSid(sid)) return false;
  std::vector<unsigned char> system;
  std::vector<unsigned char> administrators;
  std::vector<unsigned char> trusted_installer;
  if (make_well_known_sid(WinLocalSystemSid, &system) &&
      EqualSid(sid, system.data())) return true;
  if (make_well_known_sid(WinBuiltinAdministratorsSid, &administrators) &&
      EqualSid(sid, administrators.data())) return true;
  return lookup_sid(L"NT SERVICE\\TrustedInstaller", &trusted_installer) &&
         EqualSid(sid, trusted_installer.data());
}

PSID allowed_ace_sid(void *ace, unsigned char type) {
  if (type == ACCESS_ALLOWED_ACE_TYPE) {
    return &static_cast<ACCESS_ALLOWED_ACE *>(ace)->SidStart;
  }
  if (type != ACCESS_ALLOWED_OBJECT_ACE_TYPE) return nullptr;
  auto *object = static_cast<ACCESS_ALLOWED_OBJECT_ACE *>(ace);
  auto *cursor = reinterpret_cast<unsigned char *>(&object->ObjectType);
  if (object->Flags & ACE_OBJECT_TYPE_PRESENT) cursor += sizeof(GUID);
  if (object->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) cursor += sizeof(GUID);
  return cursor;
}

ACCESS_MASK allowed_ace_mask(void *ace, unsigned char type) {
  if (type == ACCESS_ALLOWED_ACE_TYPE) {
    return static_cast<ACCESS_ALLOWED_ACE *>(ace)->Mask;
  }
  if (type == ACCESS_ALLOWED_OBJECT_ACE_TYPE) {
    return static_cast<ACCESS_ALLOWED_OBJECT_ACE *>(ace)->Mask;
  }
  return 0;
}

bool secure_descriptor(const std::wstring &path, ACCESS_MASK unsafe_rights,
                       std::wstring *reason) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  unsigned long result = GetNamedSecurityInfoW(
      const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
      &dacl, nullptr, &descriptor);
  if (result != ERROR_SUCCESS) {
    *reason = L"cannot read its security descriptor (Windows error " +
              std::to_wstring(result) + L")";
    return false;
  }

  bool secure = true;
  if (! trusted_principal(owner)) {
    *reason = L"is owned by a principal other than SYSTEM, Administrators, or TrustedInstaller";
    secure = false;
  }
  else if (! dacl) {
    *reason = L"has an unrestricted (null) DACL";
    secure = false;
  }
  else {
    GENERIC_MAPPING mapping = {
        FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
        FILE_ALL_ACCESS};
    for (unsigned long index = 0; index < dacl->AceCount; ++index) {
      void *ace = nullptr;
      if (! GetAce(dacl, index, &ace)) {
        *reason = L"contains an unreadable ACL entry";
        secure = false;
        break;
      }
      auto *header = static_cast<ACE_HEADER *>(ace);
      if (header->AceFlags & INHERIT_ONLY_ACE) continue;
      PSID sid = allowed_ace_sid(ace, header->AceType);
      if (! sid || trusted_principal(sid)) continue;
      ACCESS_MASK mask = allowed_ace_mask(ace, header->AceType);
      MapGenericMask(&mask, &mapping);
      if (mask & unsafe_rights) {
        *reason = L"grants modification rights to a non-administrative principal";
        secure = false;
        break;
      }
    }
  }
  if (descriptor) LocalFree(descriptor);
  return secure;
}
bool audit_existing_object(const std::wstring &path, ACCESS_MASK unsafe_rights,
                           std::wstring *reason) {
  std::wstring detail;
  if (secure_descriptor(path, unsafe_rights, &detail)) return true;
  *reason = L"is insecure: " + path + L" " + detail;
  return false;
}

bool audit_existing_path_chain(const std::wstring &path,
                               std::wstring *reason) {
  std::wstring current = path;
  bool target = true;
  while (! current.empty()) {
    const ACCESS_MASK unsafe_rights =
        target ? kModificationRights : kAncestorReplacementRights;
    if (! audit_existing_object(current, unsafe_rights, reason)) return false;
    std::wstring parent = parent_path(current);
    if (parent.empty() || parent == current) break;
    current = std::move(parent);
    target = false;
  }
  return true;
}

}  // namespace

bool is_process_administrator() {
  std::vector<unsigned char> administrators;
  if (! make_well_known_sid(WinBuiltinAdministratorsSid, &administrators)) {
    return false;
  }
  BOOL member = FALSE;
  return CheckTokenMembership(nullptr, administrators.data(), &member) &&
         member != FALSE;
}

bool check_secure_path(const std::wstring &path, bool must_exist,
                       std::wstring *reason) {
  if (! reason || path.empty()) return false;
  reason->clear();
  std::wstring target = path;
  unsigned long attributes = GetFileAttributesW(target.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    if (must_exist) {
      *reason = L"does not exist or is inaccessible: " + path;
      return false;
    }
    target = parent_path(target);
    while (! target.empty() &&
           GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
      target = parent_path(target);
    }
    if (target.empty()) {
      *reason = L"has no accessible parent directory: " + path;
      return false;
    }
    return audit_existing_path_chain(target, reason);
  }

  return audit_existing_path_chain(target, reason);
}
