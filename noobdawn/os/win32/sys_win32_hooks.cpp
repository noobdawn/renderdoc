/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include <winsock2.h>
#include "core/core.h"
// after core.h (which pulls in windows.h) - winternl.h needs the windows types for
// RTL_USER_PROCESS_PARAMETERS / OBJECT_ATTRIBUTES used by the NtCreateUserProcess hook
#include <winternl.h>
#include "hooks/hooks.h"
#include "os/os_specific.h"
#include "strings/string_utils.h"

#include <string>

typedef int(WSAAPI *PFN_WSASTARTUP)(__in WORD wVersionRequested, __out LPWSADATA lpWSAData);
typedef int(WSAAPI *PFN_WSACLEANUP)();

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_A)(LPCSTR lpApplicationName, LPSTR lpCommandLine,
                                           LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                           LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                           BOOL bInheritHandles, DWORD dwCreationFlags,
                                           LPVOID lpEnvironment, LPCSTR lpCurrentDirectory,
                                           LPSTARTUPINFOA lpStartupInfo,
                                           LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_W)(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                           LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                           LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                           BOOL bInheritHandles, DWORD dwCreationFlags,
                                           LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
                                           LPSTARTUPINFOW lpStartupInfo,
                                           LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_AS_USER_A)(
    HANDLE hToken, LPCSTR lpApplicationName, LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_AS_USER_W)(
    HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);

typedef BOOL(WINAPI *PFN_CREATE_PROCESS_WITH_LOGON_W)(LPCWSTR lpUsername, LPCWSTR lpDomain,
                                                      LPCWSTR lpPassword, DWORD dwLogonFlags,
                                                      LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                                      DWORD dwCreationFlags, LPVOID lpEnvironment,
                                                      LPCWSTR lpCurrentDirectory,
                                                      LPSTARTUPINFOW lpStartupInfo,
                                                      LPPROCESS_INFORMATION lpProcessInformation);

// CreateProcessInternalW is undocumented but stable across Windows versions: kernel32's
// CreateProcessA/W and advapi32's CreateProcessAsUserW all forward to it. Protected launchers
// often resolve it directly from kernelbase.dll (GetProcAddress or a private export-table
// walk) to dodge IAT hooks on the documented entry points, so hooking it catches every
// in-process process-creation path.
typedef BOOL(WINAPI *PFN_CREATE_PROCESS_INTERNAL_W)(
    HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation, PHANDLE hNewToken);

// NtCreateUserProcess is the single chokepoint of all user-mode process creation - even
// CreateProcessInternalW ends up here. Protected launchers call it directly (resolving from
// ntdll themselves) to dodge hooks on the documented/kernelside layers entirely.
// PS_CREATE_INFO / PS_ATTRIBUTE_LIST are opaque to us and passed through untouched.
typedef NTSTATUS(NTAPI *PFN_NT_CREATE_USER_PROCESS)(
    PHANDLE ProcessHandle, PHANDLE ThreadHandle, ACCESS_MASK ProcessDesiredAccess,
    ACCESS_MASK ThreadDesiredAccess, POBJECT_ATTRIBUTES ProcessObjectAttributes,
    POBJECT_ATTRIBUTES ThreadObjectAttributes, ULONG ProcessFlags, ULONG ThreadFlags,
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters, PVOID CreateInfo, PVOID AttributeList);

// [NBD-DIAG] child-process injection diagnostics, gated on the same breakACE capture option
// as the diag logging in win32_hook.cpp (which keeps its own static copy of the flag).
static bool SysHookDiagEnabled()
{
  return NoobDawn::Inst().GetCaptureOptions().breakACE;
}

// [NBD-DIAG] builds a bounded UTF-8 description of a process about to be created, for logging.
static nbdstr DescribeChildTarget(LPCWSTR lpApplicationName, LPCWSTR lpCommandLine)
{
  nbdstr ret;
  if(lpApplicationName)
    ret = StringFormat::Wide2UTF8(lpApplicationName);
  if(lpCommandLine)
  {
    if(!ret.empty())
      ret += " ";
    ret += StringFormat::Wide2UTF8(lpCommandLine);
  }
  if(ret.length() > 240)
    ret = ret.substr(0, 240) + "...";
  return ret;
}

static nbdstr DescribeChildTarget(LPCSTR lpApplicationName, LPCSTR lpCommandLine)
{
  return DescribeChildTarget(
      lpApplicationName ? StringFormat::UTF82Wide(lpApplicationName).c_str() : NULL,
      lpCommandLine ? StringFormat::UTF82Wide(lpCommandLine).c_str() : NULL);
}

class SysHook : LibraryHook
{
public:
  SysHook()
  {
    // we start with a refcount of 1 because we initialise WSA ourselves for our own sockets.
    m_WSARefCount = 1;
  }

  void RegisterHooks()
  {
    NBDLOG("Registering Win32 system hooks");

    // register libraries that we care about. We don't need a callback when they are loaded
    LibraryHooks::RegisterLibraryHook("kernel32.dll", NULL);
    LibraryHooks::RegisterLibraryHook("kernelbase.dll", NULL);
    LibraryHooks::RegisterLibraryHook("ntdll.dll", NULL);
    LibraryHooks::RegisterLibraryHook("advapi32.dll", NULL);
    LibraryHooks::RegisterLibraryHook("api-ms-win-core-processthreads-l1-1-0.dll", NULL);
    LibraryHooks::RegisterLibraryHook("api-ms-win-core-processthreads-l1-1-1.dll", NULL);
    LibraryHooks::RegisterLibraryHook("api-ms-win-core-processthreads-l1-1-2.dll", NULL);
    LibraryHooks::RegisterLibraryHook("ws2_32.dll", NULL);

    // we want to hook CreateProcess purely so that we can recursively insert our hooks (if we so
    // wish)
    CreateProcessA.Register("kernel32.dll", "CreateProcessA", CreateProcessA_hook);
    CreateProcessW.Register("kernel32.dll", "CreateProcessW", CreateProcessW_hook);

    // the convergence point of all CreateProcess variants. Hooked mainly so that the breakACE
    // EAT hooking can repoint its export: targets that bypass the kernel32/advapi32 entry
    // points entirely still funnel through here.
    CreateProcessInternalW.Register("kernelbase.dll", "CreateProcessInternalW",
                                    CreateProcessInternalW_hook);

    // the ultimate chokepoint - launchers that bypass kernelbase entirely still have to come
    // here (unless they hand-roll raw syscalls, which no user-mode hook can catch)
    NtCreateUserProcess.Register("ntdll.dll", "NtCreateUserProcess", NtCreateUserProcess_hook);

    CreateProcessAsUserA.Register("advapi32.dll", "CreateProcessAsUserA", CreateProcessAsUserA_hook);
    CreateProcessAsUserW.Register("advapi32.dll", "CreateProcessAsUserW", CreateProcessAsUserW_hook);

    CreateProcessWithLogonW.Register("advapi32.dll", "CreateProcessWithLogonW",
                                     CreateProcessWithLogonW_hook);

    // handle API set exports if they exist. These don't really exist so we don't have to worry
    // about double hooking, and also they call into the 'real' implementation in kernelbase.dll
    API110CreateProcessA.Register("api-ms-win-core-processthreads-l1-1-0.dll", "CreateProcessA",
                                  API110CreateProcessA_hook);
    API110CreateProcessW.Register("api-ms-win-core-processthreads-l1-1-0.dll", "CreateProcessW",
                                  API110CreateProcessW_hook);
    API110CreateProcessAsUserW.Register("api-ms-win-core-processthreads-l1-1-0.dll",
                                        "CreateProcessAsUserW", API110CreateProcessAsUserW_hook);

    API111CreateProcessA.Register("api-ms-win-core-processthreads-l1-1-1.dll", "CreateProcessA",
                                  API111CreateProcessA_hook);
    API111CreateProcessW.Register("api-ms-win-core-processthreads-l1-1-1.dll", "CreateProcessW",
                                  API111CreateProcessW_hook);
    API111CreateProcessAsUserW.Register("api-ms-win-core-processthreads-l1-1-0.dll",
                                        "CreateProcessAsUserW", API111CreateProcessAsUserW_hook);

    API112CreateProcessA.Register("api-ms-win-core-processthreads-l1-1-2.dll", "CreateProcessA",
                                  API112CreateProcessA_hook);
    API112CreateProcessW.Register("api-ms-win-core-processthreads-l1-1-2.dll", "CreateProcessW",
                                  API112CreateProcessW_hook);
    API112CreateProcessAsUserW.Register("api-ms-win-core-processthreads-l1-1-0.dll",
                                        "CreateProcessAsUserW", API112CreateProcessAsUserW_hook);

    WSAStartup.Register("ws2_32.dll", "WSAStartup", WSAStartup_hook);
    WSACleanup.Register("ws2_32.dll", "WSACleanup", WSACleanup_hook);

    m_RecurseSlot = Threading::AllocateTLSSlot();
    Threading::SetTLSValue(m_RecurseSlot, NULL);
  }

private:
  static SysHook syshooks;

  int m_WSARefCount;
  uint64_t m_RecurseSlot = 0;

  bool CheckRecurse()
  {
    if(Threading::GetTLSValue(m_RecurseSlot) == NULL)
    {
      Threading::SetTLSValue(m_RecurseSlot, (void *)1);
      return false;
    }

    return true;
  }
  void EndRecurse() { Threading::SetTLSValue(m_RecurseSlot, NULL); }
  HookedFunction<PFN_CREATE_PROCESS_A> CreateProcessA;
  HookedFunction<PFN_CREATE_PROCESS_W> CreateProcessW;

  HookedFunction<PFN_CREATE_PROCESS_INTERNAL_W> CreateProcessInternalW;

  HookedFunction<PFN_NT_CREATE_USER_PROCESS> NtCreateUserProcess;

  HookedFunction<PFN_CREATE_PROCESS_A> API110CreateProcessA;
  HookedFunction<PFN_CREATE_PROCESS_W> API110CreateProcessW;
  HookedFunction<PFN_CREATE_PROCESS_A> API111CreateProcessA;
  HookedFunction<PFN_CREATE_PROCESS_W> API111CreateProcessW;
  HookedFunction<PFN_CREATE_PROCESS_A> API112CreateProcessA;
  HookedFunction<PFN_CREATE_PROCESS_W> API112CreateProcessW;

  HookedFunction<PFN_CREATE_PROCESS_AS_USER_A> CreateProcessAsUserA;
  HookedFunction<PFN_CREATE_PROCESS_AS_USER_W> CreateProcessAsUserW;

  HookedFunction<PFN_CREATE_PROCESS_AS_USER_W> API110CreateProcessAsUserW;
  HookedFunction<PFN_CREATE_PROCESS_AS_USER_W> API111CreateProcessAsUserW;
  HookedFunction<PFN_CREATE_PROCESS_AS_USER_W> API112CreateProcessAsUserW;

  HookedFunction<PFN_CREATE_PROCESS_WITH_LOGON_W> CreateProcessWithLogonW;

  HookedFunction<PFN_WSASTARTUP> WSAStartup;
  HookedFunction<PFN_WSACLEANUP> WSACleanup;

  static int WSAAPI WSAStartup_hook(WORD wVersionRequested, LPWSADATA lpWSAData)
  {
    int ret = syshooks.WSAStartup()(wVersionRequested, lpWSAData);

    // only increment the refcount if the function succeeded
    if(ret == 0)
      syshooks.m_WSARefCount++;

    return ret;
  }

  static int WSAAPI WSACleanup_hook()
  {
    // don't let the application murder our sockets with a mismatched WSACleanup() call
    if(syshooks.m_WSARefCount == 1)
    {
      NBDLOG("WSACleanup called with (to the application) no WSAStartup! Ignoring.");
      SetLastError(WSANOTINITIALISED);
      return SOCKET_ERROR;
    }

    // decrement refcount and call the real thing
    syshooks.m_WSARefCount--;
    return syshooks.WSACleanup()();
  }

  static BOOL WINAPI
  Hooked_CreateProcess(const char *entryPoint,
                       std::function<BOOL(DWORD dwCreationFlags, LPVOID pEnvironment,
                                          LPPROCESS_INFORMATION lpProcessInformation)>
                           realFunc,
                       DWORD dwCreationFlags, bool inject, const nbdstr &targetDesc,
                       LPVOID pEnvironment, LPPROCESS_INFORMATION lpProcessInformation)
  {
    bool recursive = syshooks.CheckRecurse();

    if(recursive)
      return realFunc(dwCreationFlags, pEnvironment, lpProcessInformation);

    PROCESS_INFORMATION dummy;
    NBDEraseEl(dummy);

    // not sure if this is valid, but I need the PID so I'll fill in my own struct to ensure that.
    if(lpProcessInformation == NULL)
    {
      lpProcessInformation = &dummy;
    }
    else
    {
      *lpProcessInformation = dummy;
    }

    bool resume = (dwCreationFlags & CREATE_SUSPENDED) == 0;
    dwCreationFlags |= CREATE_SUSPENDED;

    nbdstr envA;
    std::wstring envW;
    void *env = pEnvironment;
    const bool unicode_env = (dwCreationFlags & CREATE_UNICODE_ENVIRONMENT) != 0;

// give ourselves access to the ANSI version if we want it
#undef GetEnvironmentStrings

    static_assert(std::is_same<decltype(GetEnvironmentStrings()), char *>::value,
                  "GetEnvironmentStrings macro is messing up");

    // if we have no existing environment, take it from the current env strings that will be used
    // implicitly so we can patch it
    if(!env)
      env = unicode_env ? (void *)GetEnvironmentStringsW() : (void *)GetEnvironmentStrings();

    // patch the environment string to remove vulkan layer variable
    if(unicode_env)
    {
      const wchar_t *cur = (const wchar_t *)env;

      // loop over every A=B\0 string
      while(*cur)
      {
        // if it is NOT the vulkan env var, append it to our block
        if(wcsncmp(cur, CONCAT(L, NOOBDAWN_VULKAN_LAYER_VAR), sizeof(NOOBDAWN_VULKAN_LAYER_VAR) - 1))
        {
          envW += cur;
          envW.push_back(L'\0');
        }

        cur += wcslen(cur) + 1;
      }

      // append the extra \0 to terminate the block
      envW.push_back(L'\0');

      // use the patched block
      env = (void *)envW.data();
    }
    else
    {
      const char *cur = (const char *)env;

      // loop over every A=B\0 string
      while(*cur)
      {
        // if it is NOT the vulkan env var, append it to our block
        if(strncmp(cur, NOOBDAWN_VULKAN_LAYER_VAR, sizeof(NOOBDAWN_VULKAN_LAYER_VAR) - 1))
        {
          envA += cur;
          envA.push_back('\0');
        }

        cur += strlen(cur) + 1;
      }

      // append the extra \0 to terminate the block
      envA.push_back('\0');

      // use the patched block
      env = (void *)envA.data();
    }

    NBDDEBUG("Calling real %s", entryPoint);
    BOOL ret = realFunc(dwCreationFlags, env, lpProcessInformation);
    NBDDEBUG("Called real %s", entryPoint);

    // [NBD-DIAG] log every child process creation that comes through our hooks, so we can see
    // exactly which processes a protected launcher spawns and what we decided to do with them
    if(SysHookDiagEnabled())
    {
      if(!ret)
        NBDLOG("[NBD-DIAG] %s failed (GetLastError %u) creating child '%s'", entryPoint,
               GetLastError(), targetDesc.c_str());
      else if(inject)
        NBDLOG("[NBD-DIAG] %s created child pid %u '%s' - injecting", entryPoint,
               lpProcessInformation->dwProcessId, targetDesc.c_str());
      else
        NBDLOG("[NBD-DIAG] %s created child pid %u '%s' - not injecting", entryPoint,
               lpProcessInformation->dwProcessId, targetDesc.c_str());
    }

    if(ret && inject)
    {
      NBDDEBUG("Intercepting %s", entryPoint);

      // inherit logfile and capture options
      nbdpair<RDResult, uint32_t> res = Process::InjectIntoProcess(
          lpProcessInformation->dwProcessId, {}, NoobDawn::Inst().GetCaptureFileTemplate(),
          NoobDawn::Inst().GetCaptureOptions(), NoobDawn::Inst().GetBlacklist(), false);

      // [NBD-DIAG] previously injection failure here was completely silent
      if(res.first == ResultCode::Succeeded)
      {
        NoobDawn::Inst().AddChildProcess((uint32_t)lpProcessInformation->dwProcessId, res.second);

        if(SysHookDiagEnabled())
          NBDLOG("[NBD-DIAG] child pid %u injection succeeded (target control ident %u)",
                 lpProcessInformation->dwProcessId, res.second);
      }
      else if(SysHookDiagEnabled())
      {
        NBDERR("[NBD-DIAG] child pid %u injection FAILED (code %u): %s",
               lpProcessInformation->dwProcessId, (uint32_t)res.first.code,
               res.first.message.c_str());
      }
    }

    if(resume)
    {
      ResumeThread(lpProcessInformation->hThread);
    }

    // ensure we clean up after ourselves
    if(dummy.dwProcessId != 0)
    {
      CloseHandle(dummy.hProcess);
      CloseHandle(dummy.hThread);
    }

    syshooks.EndRecurse();

    return ret;
  }

  static bool ShouldInject(LPCWSTR lpApplicationName, LPCWSTR lpCommandLine)
  {
    if(!NoobDawn::Inst().GetCaptureOptions().hookIntoChildren)
    {
      // [NBD-DIAG] previously this early-out was silent
      if(SysHookDiagEnabled())
        NBDLOG("[NBD-DIAG] ShouldInject: hookIntoChildren is disabled, child '%s' skipped",
               DescribeChildTarget(lpApplicationName, lpCommandLine).c_str());
      return false;
    }

    const CaptureOptions &capOpts = NoobDawn::Inst().GetCaptureOptions();

    // whitelist mode: the user-supplied process list is inverted - only children matching one
    // of its entries get injected. Requires enableBlacklist (the master switch for the list)
    // so the UI can present it as a modifier of the list rather than an independent mode.
    if(capOpts.enableBlacklist && capOpts.enableWhitelist)
    {
      nbdstr app =
          lpApplicationName ? strlower(StringFormat::Wide2UTF8(lpApplicationName)) : nbdstr();
      nbdstr cmd = lpCommandLine ? strlower(StringFormat::Wide2UTF8(lpCommandLine)) : nbdstr();

      // hard exclusions always apply, in both modes - never inject our own tools
      static const char *hardExcluded[] = {"noobdawncmd.exe", "qnoobdawn.exe"};
      for(const char *black : hardExcluded)
      {
        if((!app.empty() && app.contains(black)) || (!cmd.empty() && cmd.contains(black)))
          return false;
      }

      nbdstr whitestr = NoobDawn::Inst().GetBlacklist();
      nbdarray<nbdstr> whitelist;
      split(whitestr, whitelist, ';');

      for(const nbdstr &white : whitelist)
      {
        nbdstr w = strlower(white.trimmed());
        if(w.empty())
          continue;

        if((!app.empty() && app.contains(w)) || (!cmd.empty() && cmd.contains(w)))
        {
          if(SysHookDiagEnabled())
            NBDLOG("[NBD-DIAG] ShouldInject: child '%s' matches whitelist entry '%s' - injecting",
                   DescribeChildTarget(lpApplicationName, lpCommandLine).c_str(), w.c_str());
          return true;
        }
      }

      if(SysHookDiagEnabled())
        NBDLOG("[NBD-DIAG] ShouldInject: child '%s' matches no whitelist entry - skipped",
               DescribeChildTarget(lpApplicationName, lpCommandLine).c_str());
      return false;
    }

    // processes that must never be injected into, to avoid infinite recursion.
    // if the blacklist option is enabled, user-supplied process names are added.
    nbdarray<nbdstr> blacklist = {"noobdawncmd.exe", "qnoobdawn.exe"};
    if(NoobDawn::Inst().GetCaptureOptions().enableBlacklist)
    {
      nbdstr blackstr = NoobDawn::Inst().GetBlacklist();
      nbdarray<nbdstr> customList;
      split(blackstr, customList, ';');
      for(const nbdstr &black : customList)
      {
        if(!black.trimmed().empty())
          blacklist.push_back(strlower(black.trimmed()));
      }
    }

    bool inject = true;

    // sanity check to make sure we're not going to go into an infinity loop injecting into
    // ourselves.
    if(lpApplicationName)
    {
      nbdstr app = strlower(StringFormat::Wide2UTF8(lpApplicationName));

      for(const nbdstr &black : blacklist)
      {
        if(app.contains(black))
        {
          NBDDEBUG("%s contains %s (blacklist)", app.c_str(), black.c_str());
          // [NBD-DIAG] surface blacklist rejections in the normal log
          if(SysHookDiagEnabled())
            NBDLOG("[NBD-DIAG] ShouldInject: child '%s' matches blacklist entry '%s' - skipped",
                   DescribeChildTarget(lpApplicationName, lpCommandLine).c_str(), black.c_str());
          inject = false;
          break;
        }
      }
    }
    if(lpCommandLine)
    {
      nbdstr cmd = strlower(StringFormat::Wide2UTF8(lpCommandLine));

      for(const nbdstr &black : blacklist)
      {
        if(cmd.contains(black))
        {
          NBDDEBUG("%s contains %s (blacklist)", cmd.c_str(), black.c_str());
          // [NBD-DIAG] surface blacklist rejections in the normal log
          if(SysHookDiagEnabled())
            NBDLOG("[NBD-DIAG] ShouldInject: child '%s' matches blacklist entry '%s' - skipped",
                   DescribeChildTarget(lpApplicationName, lpCommandLine).c_str(), black.c_str());
          inject = false;
          break;
        }
      }
    }

    return inject;
  }

  static bool ShouldInject(LPCSTR lpApplicationName, LPCSTR lpCommandLine)
  {
    if(!NoobDawn::Inst().GetCaptureOptions().hookIntoChildren)
      return false;

    return ShouldInject(lpApplicationName ? StringFormat::UTF82Wide(lpApplicationName).c_str() : NULL,
                        lpCommandLine ? StringFormat::UTF82Wide(lpCommandLine).c_str() : NULL);
  }

  static BOOL WINAPI CreateProcessA_hook(
      __in_opt LPCSTR lpApplicationName, __inout_opt LPSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCSTR lpCurrentDirectory,
      __in LPSTARTUPINFOA lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessA()(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                           lpThreadAttributes, bInheritHandles, flags, env,
                                           lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI CreateProcessW_hook(__in_opt LPCWSTR lpApplicationName,
                                         __inout_opt LPWSTR lpCommandLine,
                                         __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
                                         __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes,
                                         __in BOOL bInheritHandles, __in DWORD dwCreationFlags,
                                         __in_opt LPVOID lpEnvironment,
                                         __in_opt LPCWSTR lpCurrentDirectory,
                                         __in LPSTARTUPINFOW lpStartupInfo,
                                         __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessW()(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                           lpThreadAttributes, bInheritHandles, flags, env,
                                           lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI CreateProcessInternalW_hook(
      HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
      LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
      LPPROCESS_INFORMATION lpProcessInformation, PHANDLE hNewToken)
  {
    // when the extended hook scope option is off this hook is completely transparent - only
    // the documented CreateProcess variants do child injection then
    if(!NoobDawn::Inst().GetCaptureOptions().extendedHookScope)
      return syshooks.CreateProcessInternalW()(
          hUserToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
          bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo,
          lpProcessInformation, hNewToken);

    return Hooked_CreateProcess(
        "CreateProcessInternalW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessInternalW()(
              hUserToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi, hNewToken);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static NTSTATUS NTAPI NtCreateUserProcess_hook(
      PHANDLE ProcessHandle, PHANDLE ThreadHandle, ACCESS_MASK ProcessDesiredAccess,
      ACCESS_MASK ThreadDesiredAccess, POBJECT_ATTRIBUTES ProcessObjectAttributes,
      POBJECT_ATTRIBUTES ThreadObjectAttributes, ULONG ProcessFlags, ULONG ThreadFlags,
      PRTL_USER_PROCESS_PARAMETERS ProcessParameters, PVOID CreateInfo, PVOID AttributeList)
  {
    // when the extended hook scope option is off this hook is completely transparent
    if(!NoobDawn::Inst().GetCaptureOptions().extendedHookScope)
      return syshooks.NtCreateUserProcess()(ProcessHandle, ThreadHandle, ProcessDesiredAccess,
                                            ThreadDesiredAccess, ProcessObjectAttributes,
                                            ThreadObjectAttributes, ProcessFlags, ThreadFlags,
                                            ProcessParameters, CreateInfo, AttributeList);

    // nested hits (CreateProcessW -> CreateProcessInternalW -> here) must pass straight
    // through: the outermost hooked layer already injected this child, and injecting again
    // would register it twice
    if(syshooks.CheckRecurse())
      return syshooks.NtCreateUserProcess()(ProcessHandle, ThreadHandle, ProcessDesiredAccess,
                                            ThreadDesiredAccess, ProcessObjectAttributes,
                                            ThreadObjectAttributes, ProcessFlags, ThreadFlags,
                                            ProcessParameters, CreateInfo, AttributeList);

    // THREAD_CREATE_FLAGS_CREATE_SUSPENDED - force the initial thread suspended (mirroring
    // what Hooked_CreateProcess does with CREATE_SUSPENDED) so we can inject before any of
    // the child's code runs, then resume afterwards
    const ULONG threadCreateFlagsSuspended = 0x1;

    // the image path lives in the process parameters - direct Nt callers don't have the nice
    // lpApplicationName/lpCommandLine split that the Win32 APIs give us
    LPCWSTR imagePath =
        (ProcessParameters && ProcessParameters->ImagePathName.Buffer)
            ? ProcessParameters->ImagePathName.Buffer
            : NULL;

    nbdstr targetDesc = DescribeChildTarget(imagePath, (LPCWSTR)NULL);
    bool inject = ShouldInject(imagePath, (LPCWSTR)NULL);

    if(SysHookDiagEnabled())
      NBDLOG("[NBD-DIAG] NtCreateUserProcess: '%s'%s", targetDesc.c_str(),
             inject ? "" : " (not injecting)");

    bool resume = (ThreadFlags & threadCreateFlagsSuspended) == 0;
    ThreadFlags |= threadCreateFlagsSuspended;

    NTSTATUS status = syshooks.NtCreateUserProcess()(
        ProcessHandle, ThreadHandle, ProcessDesiredAccess, ThreadDesiredAccess,
        ProcessObjectAttributes, ThreadObjectAttributes, ProcessFlags, ThreadFlags,
        ProcessParameters, CreateInfo, AttributeList);

    if(status >= 0 && ProcessHandle && *ProcessHandle)
    {
      DWORD pid = GetProcessId(*ProcessHandle);

      if(SysHookDiagEnabled())
        NBDLOG("[NBD-DIAG] NtCreateUserProcess created child pid %u '%s'%s", pid,
               targetDesc.c_str(), inject ? " - injecting" : " - not injecting");

      if(inject && pid != 0)
      {
        // inherit logfile and capture options, same as Hooked_CreateProcess
        nbdpair<RDResult, uint32_t> res = Process::InjectIntoProcess(
            pid, {}, NoobDawn::Inst().GetCaptureFileTemplate(),
            NoobDawn::Inst().GetCaptureOptions(), NoobDawn::Inst().GetBlacklist(), false);

        if(res.first == ResultCode::Succeeded)
        {
          NoobDawn::Inst().AddChildProcess(pid, res.second);

          if(SysHookDiagEnabled())
            NBDLOG("[NBD-DIAG] child pid %u injection succeeded (target control ident %u)", pid,
                   res.second);
        }
        else if(SysHookDiagEnabled())
        {
          NBDERR("[NBD-DIAG] child pid %u injection FAILED (code %u): %s", pid,
                 (uint32_t)res.first.code, res.first.message.c_str());
        }
      }

      if(resume && ThreadHandle && *ThreadHandle)
        ResumeThread(*ThreadHandle);
    }
    else if(SysHookDiagEnabled())
    {
      NBDLOG("[NBD-DIAG] NtCreateUserProcess failed (status 0x%x) for '%s'", (uint32_t)status,
             targetDesc.c_str());
    }

    syshooks.EndRecurse();

    return status;
  }

  static BOOL WINAPI API110CreateProcessA_hook(
      __in_opt LPCSTR lpApplicationName, __inout_opt LPSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCSTR lpCurrentDirectory,
      __in LPSTARTUPINFOA lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API110CreateProcessA()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API110CreateProcessW_hook(
      __in_opt LPCWSTR lpApplicationName, __inout_opt LPWSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCWSTR lpCurrentDirectory,
      __in LPSTARTUPINFOW lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API110CreateProcessW()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API111CreateProcessA_hook(
      __in_opt LPCSTR lpApplicationName, __inout_opt LPSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCSTR lpCurrentDirectory,
      __in LPSTARTUPINFOA lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API111CreateProcessA()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API111CreateProcessW_hook(
      __in_opt LPCWSTR lpApplicationName, __inout_opt LPWSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCWSTR lpCurrentDirectory,
      __in LPSTARTUPINFOW lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API111CreateProcessW()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API112CreateProcessA_hook(
      __in_opt LPCSTR lpApplicationName, __inout_opt LPSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCSTR lpCurrentDirectory,
      __in LPSTARTUPINFOA lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API112CreateProcessA()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API112CreateProcessW_hook(
      __in_opt LPCWSTR lpApplicationName, __inout_opt LPWSTR lpCommandLine,
      __in_opt LPSECURITY_ATTRIBUTES lpProcessAttributes,
      __in_opt LPSECURITY_ATTRIBUTES lpThreadAttributes, __in BOOL bInheritHandles,
      __in DWORD dwCreationFlags, __in_opt LPVOID lpEnvironment, __in_opt LPCWSTR lpCurrentDirectory,
      __in LPSTARTUPINFOW lpStartupInfo, __out LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API112CreateProcessW()(
              lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI CreateProcessAsUserA_hook(
      HANDLE hToken, LPCSTR lpApplicationName, LPSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory,
      LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserA",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessAsUserA()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI CreateProcessAsUserW_hook(
      HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
      LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessAsUserW()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI CreateProcessWithLogonW_hook(LPCWSTR lpUsername, LPCWSTR lpDomain,
                                                  LPCWSTR lpPassword, DWORD dwLogonFlags,
                                                  LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                                                  DWORD dwCreationFlags, LPVOID lpEnvironment,
                                                  LPCWSTR lpCurrentDirectory,
                                                  LPSTARTUPINFOW lpStartupInfo,
                                                  LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.CreateProcessWithLogonW()(lpUsername, lpDomain, lpPassword, dwLogonFlags,
                                                    lpApplicationName, lpCommandLine, flags, env,
                                                    lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API110CreateProcessAsUserW_hook(
      HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
      LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API110CreateProcessAsUserW()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API111CreateProcessAsUserW_hook(
      HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
      LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API111CreateProcessAsUserW()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }

  static BOOL WINAPI API112CreateProcessAsUserW_hook(
      HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
      LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
      BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
      LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
  {
    return Hooked_CreateProcess(
        "CreateProcessAsUserW",
        [=](DWORD flags, LPVOID env, LPPROCESS_INFORMATION pi) {
          return syshooks.API112CreateProcessAsUserW()(
              hToken, lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
              bInheritHandles, flags, env, lpCurrentDirectory, lpStartupInfo, pi);
        },
        dwCreationFlags, ShouldInject(lpApplicationName, lpCommandLine),
        DescribeChildTarget(lpApplicationName, lpCommandLine), lpEnvironment,
        lpProcessInformation);
  }
};

SysHook SysHook::syshooks;
