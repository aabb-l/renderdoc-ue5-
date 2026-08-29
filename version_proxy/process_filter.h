#pragma once

#include <wchar.h>

inline bool ShouldEnableProxyInjectionForProcessPath(const wchar_t *processPath)
{
  const wchar_t *exeName = processPath ? processPath : L"";

  for(const wchar_t *cursor = exeName; *cursor; ++cursor)
  {
    if(*cursor == L'\\' || *cursor == L'/')
      exeName = cursor + 1;
  }

  const wchar_t *renderDocTools[] = {
      L"qrendertest.exe",
      L"rendertestcmd.exe",
      L"qrenderdoc.exe",
      L"renderdoccmd.exe",
  };

  for(const wchar_t *toolName : renderDocTools)
  {
    if(_wcsicmp(exeName, toolName) == 0)
      return false;
  }

  return true;
}
