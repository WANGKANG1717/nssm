#include "events.h"

#include "output.h"
#include "win32_handle.h"

#include <windows.h>
#include <winevt.h>

#include <string>
#include <vector>

namespace {

struct unique_event_query {
  EVT_HANDLE value = nullptr;
  ~unique_event_query() { if (value) EvtClose(value); }
};

bool render_event_xml(EVT_HANDLE event, std::wstring *xml,
                      unsigned long *error) {
  unsigned long bytes = 0;
  unsigned long properties = 0;
  EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &bytes,
            &properties);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || ! bytes) {
    if (error) *error = GetLastError();
    return false;
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  if (! EvtRender(nullptr, event, EvtRenderEventXml, bytes, buffer.data(),
                  &bytes, &properties)) {
    if (error) *error = GetLastError();
    return false;
  }
  *xml = buffer.data();
  return true;
}

}  // namespace

int events_command(int argc, wchar_t **argv) {
  bool json = false;
  unsigned long limit = 50;
  std::wstring service;
  for (int index = 0; index < argc; ++index) {
    if (! wcscmp(argv[index], L"--output=json") ||
        (! wcscmp(argv[index], L"-o") && index + 1 < argc &&
         ! wcscmp(argv[index + 1], L"json"))) {
      json = true;
      if (! wcscmp(argv[index], L"-o")) ++index;
    }
    else if ((! wcscmp(argv[index], L"--lines") ||
              ! wcscmp(argv[index], L"-n")) && index + 1 < argc) {
      wchar_t *end = nullptr;
      limit = wcstoul(argv[++index], &end, 10);
      if (! end || *end || ! limit || limit > 1000) return 2;
    }
    else if (argv[index][0] == L'-' || ! service.empty()) return 2;
    else service = argv[index];
  }
  if (service.empty()) {
    print_cli_error(json, L"usage",
                    L"Usage: svp events [-n N|--lines N] [-o json|--output=json] <service>");
    return 2;
  }

  unique_event_query query;
  query.value = EvtQuery(nullptr, L"Application",
      L"*[System[Provider[@Name='SvcPilot']]]",
      EvtQueryChannelPath | EvtQueryReverseDirection);
  if (! query.value) {
    print_cli_error(json, L"system", L"Unable to query the Windows event log",
                    GetLastError());
    return 5;
  }
  if (json) fputws(L"{\"ok\":true,\"operation\":\"events\",\"service\":", stdout);
  if (json) { write_json_string(stdout, service); fputws(L",\"events\":[", stdout); }
  unsigned long matched = 0;
  bool first = true;
  while (matched < limit) {
    EVT_HANDLE event = nullptr;
    unsigned long returned = 0;
    if (! EvtNext(query.value, 1, &event, 0, 0, &returned)) {
      const unsigned long error = GetLastError();
      if (error == ERROR_NO_MORE_ITEMS) break;
      if (json) fputws(L"]}\n", stdout);
      return 5;
    }
    unique_event_query event_guard{event};
    std::wstring xml;
    unsigned long error = ERROR_SUCCESS;
    if (! render_event_xml(event, &xml, &error)) continue;
    if (xml.find(service) == std::wstring::npos) continue;
    if (json) {
      if (! first) fputwc(L',', stdout);
      write_json_string(stdout, xml);
    }
    else fwprintf(stdout, L"%ls\n", xml.c_str());
    first = false;
    ++matched;
  }
  if (json) fputws(L"]}\n", stdout);
  return 0;
}
