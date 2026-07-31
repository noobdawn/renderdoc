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

// must be separate so that it's included first and not sorted by clang-format
#include <windows.h>

#include <tlhelp32.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include "common/common.h"
#include "common/threading.h"
#include "hooks/hooks.h"
#include "os/os_specific.h"
#include "strings/string_utils.h"

#define VERBOSE_DEBUG_HOOK OPTION_OFF

// map from address of IAT entry, to original contents
std::map<void **, void *> s_InstalledHooks;
Threading::CriticalSection installedLock;

// when false (the default), all of the breakACE behaviour below is disabled and the original
// hooking logic runs unchanged. Set via Win32_CaptureOptionsUpdated() from the capture options.
static bool s_BreakACE = false;

// [NBD-DIAG] separately gates the kernelbase/ntdll parts of the EAT hook scope - see
// CaptureOptions::extendedHookScope
static bool s_ExtendedHookScope = false;

// [NBD-DIAG] temporary diagnostics for investigating protected targets where graphics hooks
// never trigger. All diag lines are prefixed with [NBD-DIAG] for easy filtering.
static bool DiagLogOnce(const nbdstr &key)
{
  if(!s_BreakACE)
    return false;

  static Threading::CriticalSection diaglock;
  static std::set<nbdstr> seen;
  SCOPED_LOCK(diaglock);
  if(seen.find(key) != seen.end())
    return false;
  seen.insert(key);
  return true;
}

static bool IsGraphicsDLLName(const nbdstr &lower)
{
  const char *s = lower.c_str();
  return strstr(s, "d3d") || strstr(s, "dxgi") || strstr(s, "vulkan") || strstr(s, "opengl") ||
         strstr(s, "egl") || strstr(s, "gles") || strstr(s, "nvapi") || strstr(s, "nvogl") ||
         strstr(s, "atiogl") || strstr(s, "amdvlk") || strstr(s, "igvk") || strstr(s, "igd") ||
         strstr(s, "d3dcompiler");
}

static nbdstr CachedModuleBasenameLower(HMODULE mod)
{
  static Threading::CriticalSection modnamelock;
  static std::map<HMODULE, nbdstr> cache;
  SCOPED_LOCK(modnamelock);
  auto it = cache.find(mod);
  if(it != cache.end())
    return it->second;

  char path[MAX_PATH] = {};
  GetModuleFileNameA(mod, path, MAX_PATH - 1);
  const char *slash = strrchr(path, '\\');
  nbdstr base = strlower(nbdstr(slash ? slash + 1 : path));
  cache[mod] = base;
  return base;
}

bool ApplyHook(FunctionHook &hook, void **IATentry, bool &already)
{
  DWORD oldProtection = PAGE_EXECUTE;

  if(*IATentry == hook.hook)
  {
    already = true;
    return true;
  }

#if ENABLED(VERBOSE_DEBUG_HOOK)
  NBDDEBUG("Patching IAT for %s: %p to %p", hook.function.c_str(), IATentry, hook.hook);
#endif

  {
    SCOPED_LOCK(installedLock);
    if(s_InstalledHooks.find(IATentry) == s_InstalledHooks.end())
      s_InstalledHooks[IATentry] = *IATentry;
  }

  BOOL success = VirtualProtect(IATentry, sizeof(void *), PAGE_READWRITE, &oldProtection);
  if(!success)
  {
    NBDERR("Failed to make IAT entry writeable 0x%p", IATentry);
    return false;
  }

  *IATentry = hook.hook;

  success = VirtualProtect(IATentry, sizeof(void *), oldProtection, &oldProtection);
  if(!success)
  {
    NBDERR("Failed to restore IAT entry protection 0x%p", IATentry);
    return false;
  }

  return true;
}

// [NBD-DIAG] EAT (export address table) hooking state. Instead of only patching callers'
// import tables, we additionally rewrite the exports of key graphics DLLs at the source, so
// that ANY resolution path (static import, GetProcAddress, LdrGetProcedureAddress, delay-load
// helper, or a protector's private export walker) yields our wrapper. This is essential for
// packed/protected targets whose import directory we cannot walk.
struct EATEntry
{
  DWORD *slot;
  DWORD originalRVA;
  DWORD hookRVA;
};

struct DllHookset
{
  HMODULE module = NULL;
  bool hooksfetched = false;
  // if we have multiple copies of the dll loaded (unlikely), the other module handles will be
  // stored here
  nbdarray<HMODULE> altmodules;
  nbdarray<FunctionHook> FunctionHooks;
  DWORD OrdinalBase = 0;
  nbdarray<nbdstr> OrdinalNames;
  nbdarray<FunctionLoadCallback> Callbacks;
  Threading::CriticalSection ordinallock;

  // [NBD-DIAG] EAT hook state
  bool eatAttempted = false;
  void *eatStubPage = NULL;
  size_t eatStubUsed = 0;
  nbdarray<EATEntry> eatEntries;

  void FetchOrdinalNames()
  {
    SCOPED_LOCK(ordinallock);

    // return if we already fetched the ordinals
    if(!OrdinalNames.empty())
      return;

    byte *baseAddress = (byte *)module;

#if ENABLED(VERBOSE_DEBUG_HOOK)
    NBDDEBUG("FetchOrdinalNames");
#endif

    PIMAGE_DOS_HEADER dosheader = (PIMAGE_DOS_HEADER)baseAddress;

    if(dosheader->e_magic != 0x5a4d)
      return;

    char *PE00 = (char *)(baseAddress + dosheader->e_lfanew);
    PIMAGE_FILE_HEADER fileHeader = (PIMAGE_FILE_HEADER)(PE00 + 4);
    PIMAGE_OPTIONAL_HEADER optHeader =
        (PIMAGE_OPTIONAL_HEADER)((BYTE *)fileHeader + sizeof(IMAGE_FILE_HEADER));

    DWORD eatOffset = optHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;

    IMAGE_EXPORT_DIRECTORY *exportDesc = (IMAGE_EXPORT_DIRECTORY *)(baseAddress + eatOffset);

    WORD *ordinals = (WORD *)(baseAddress + exportDesc->AddressOfNameOrdinals);
    DWORD *names = (DWORD *)(baseAddress + exportDesc->AddressOfNames);

    DWORD count = NBDMIN(exportDesc->NumberOfFunctions, exportDesc->NumberOfNames);

    WORD maxOrdinal = 0;
    for(DWORD i = 0; i < count; i++)
      maxOrdinal = NBDMAX(maxOrdinal, ordinals[i]);

    OrdinalBase = exportDesc->Base;
    OrdinalNames.resize(maxOrdinal + 1);

    for(DWORD i = 0; i < count; i++)
    {
      OrdinalNames[ordinals[i]] = (char *)(baseAddress + names[i]);

#if ENABLED(VERBOSE_DEBUG_HOOK)
      NBDDEBUG("ordinal found: '%s' %u", OrdinalNames[ordinals[i]].c_str(), (uint32_t)ordinals[i]);
#endif
    }
  }
};

// [NBD-DIAG] only core graphics API DLLs plus kernelbase.dll get EAT hooked. kernelbase is
// included because CreateProcessInternalW is the convergence point of all process creation on
// Windows, and protected launchers often resolve it directly (GetProcAddress or their own
// export-table walk) to dodge IAT hooks on kernel32/advapi32. Other system modules are still
// deliberately excluded: redirecting their exports process-wide would change behaviour for
// modules that RenderDoc intentionally excludes from hooking.
static bool IsEATHookTarget(const nbdstr &dllName)
{
  nbdstr lower = strlower(dllName);

  // kernelbase/ntdll only when the extended hook scope option is on - rewriting exports of
  // the lowest-level system DLLs is a much bigger behavioural change than graphics DLLs
  if(lower == "kernelbase.dll" || lower == "ntdll.dll")
    return s_ExtendedHookScope;

  return lower == "d3d9.dll" || lower == "d3d11.dll" || lower == "d3d12.dll" ||
         lower == "dxgi.dll" || lower == "opengl32.dll" || lower == "vulkan-1.dll" ||
         lower == "libegl.dll" || lower == "libglesv2.dll";
}

// allocate an RWX page above the module so that 32-bit EAT RVAs (module base relative) can
// reach the stubs
static void *AllocStubAbove(HMODULE module)
{
  uintptr_t base = (uintptr_t)module;

  for(uintptr_t addr = (base + 0x100000) & ~uintptr_t(0xFFFF); addr < base + 0xFFF00000;
      addr += 0x10000)
  {
    void *p = VirtualAlloc((LPVOID)addr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if(p)
      return p;
  }

  return NULL;
}

static void ApplyEATHooksToHookset(DllHookset &hookset, const nbdstr &dllName);

struct CachedHookData
{
  bool hookAll = true;

  std::map<nbdstr, DllHookset> DllHooks;
  HMODULE ownmodule = NULL;
  Threading::CriticalSection lock;

  std::set<nbdstr> ignores;

  bool missedOrdinals = false;
  std::function<HMODULE(const nbdstr &, HANDLE, DWORD)> libraryIntercept;

  int32_t posthooking = 0;

  void ApplyHooks(const char *modName, HMODULE module)
  {
    char lowername[512] = {};

    {
      size_t i = 0;
      while(modName[i])
      {
        lowername[i] = (char)tolower(modName[i]);
        i++;
      }
      lowername[i] = 0;
    }

    // [NBD-DIAG] temporary diagnostic: log each module we ever scan (deduped)
    if(DiagLogOnce(nbdstr("MOD:") + lowername))
      NBDLOG("[NBD-DIAG] Module seen: %s", modName);

#if ENABLED(VERBOSE_DEBUG_HOOK)
    NBDDEBUG("=== ApplyHooks(%s, %p)", modName, module);
#endif

    // fraps seems to non-safely modify the assembly around the hook function, if
    // we modify its import descriptors it leads to a crash as it hooks OUR functions.
    // instead, skip modifying the import descriptors, it will hook the 'real' d3d functions
    // and we can call them and have fraps + noobdawn playing nicely together.
    // we also exclude some other overlay renderers here, such as steam's
    //
    // Also we exclude ourselves here - just in case the application has already loaded
    // noobdawn.dll, or tries to load it.
    if(strstr(lowername, "fraps") || strstr(lowername, "gameoverlayrenderer") ||
       strstr(lowername, STRINGIZE(RDOC_BASE_NAME) ".dll") == lowername)
      return;

    // set module pointer if we are hooking exports from this module
    for(auto it = DllHooks.begin(); it != DllHooks.end(); ++it)
    {
      if(!_stricmp(it->first.c_str(), modName))
      {
        if(it->second.module == NULL)
        {
          it->second.module = module;

          it->second.hooksfetched = true;

          // fetch all function hooks here, since we want to fill out the original function pointer
          // even in case nothing imports from that function (which means it would not get filled
          // out through FunctionHook::ApplyHook)
          for(FunctionHook &hook : it->second.FunctionHooks)
          {
            if(hook.orig && *hook.orig == NULL)
              *hook.orig = GetProcAddress(module, hook.function.c_str());
          }

          it->second.FetchOrdinalNames();

          // [NBD-DIAG] temporary diagnostic: confirm when a hooked DLL is seen loaded
          if(s_BreakACE)
            NBDLOG("[NBD-DIAG] Hooked DLL present: %s @ 0x%p", it->first.c_str(), module);

          // [NBD-DIAG] apply EAT hooks for graphics DLLs
          ApplyEATHooksToHookset(it->second, it->first);
        }
        else if(it->second.module != module)
        {
          // if it's already in altmodules, bail
          bool already = false;

          for(size_t i = 0; i < it->second.altmodules.size(); i++)
          {
            if(it->second.altmodules[i] == module)
            {
              already = true;
              break;
            }
          }

          if(already)
            break;

          // check if the previous module is still valid
          SetLastError(0);
          char filename[MAX_PATH] = {};
          GetModuleFileNameA(it->second.module, filename, MAX_PATH - 1);
          DWORD err = GetLastError();
          char *slash = strrchr(filename, L'\\');

          nbdstr basename = slash ? strlower(nbdstr(slash + 1)) : "";

          if(err == 0 && basename == it->first)
          {
            // previous module is still loaded, add this to the alt modules list
            it->second.altmodules.push_back(module);
          }
          else
          {
            // previous module is no longer loaded or there's a new file there now, add this as the
            // new location
            NBDWARN("%s moved from %p to %p, re-initialising orig pointers", it->first.c_str(),
                    it->second.module, module);

            // we also need to re-initialise the hooks as the orig pointers are now stale
            for(FunctionHook &hook : it->second.FunctionHooks)
            {
              if(hook.orig)
                *hook.orig = GetProcAddress(module, hook.function.c_str());
            }

            it->second.module = module;
          }
        }
      }
    }

    // for safety (and because we don't need to), ignore these modules
    if(!_stricmp(modName, "kernel32.dll") || !_stricmp(modName, "powrprof.dll") ||
       !_stricmp(modName, "CoreMessaging.dll") || !_stricmp(modName, "opengl32.dll") ||
       !_stricmp(modName, "gdi32.dll") || !_stricmp(modName, "gdi32full.dll") ||
       !_stricmp(modName, "windows.storage.dll") || !_stricmp(modName, "nvoglv32.dll") ||
       !_stricmp(modName, "nvoglv64.dll") || !_stricmp(modName, "vulkan-1.dll") ||
       !_stricmp(modName, "atio6axx.dll") || !_stricmp(modName, "atioglxx.dll") ||
       !_stricmp(modName, "nvcuda.dll") || strstr(lowername, "cudart") == lowername ||
       strstr(lowername, "msvcr") == lowername || strstr(lowername, "msvcp") == lowername ||
       strstr(lowername, "nv-vk") == lowername || strstr(lowername, "amdvlk") == lowername ||
       strstr(lowername, "igvk") == lowername || strstr(lowername, "nvopencl") == lowername ||
       strstr(lowername, "nvapi") == lowername)
      return;

    if(ignores.find(lowername) != ignores.end())
      return;

    // the module could have been unloaded after our toolhelp snapshot, especially if we spent a
    // long time
    // dealing with a previous module (like adding our hooks).
    wchar_t modpath[1024] = {0};
    GetModuleFileNameW(module, modpath, 1023);
    if(modpath[0] == 0)
      return;

    // windows 11 and newer versions have weird hotpatch DLLs that don't act like real DLLs. The
    // LoadLibraryW below will fail for these DLLs even when using the module path provided.
    // Only check the path for DLLs that might be a windows-hotpatch but if it matches we'll skip
    // hooking these to avoid problems
    if(strstr(lowername, "hotpatch"))
    {
      wchar_t lowerpath[1024] = {};

      size_t i = 0;
      while(modpath[i])
      {
        lowerpath[i] = towlower(modpath[i]);
        i++;
      }
      lowerpath[i] = 0;

      if(wcsstr(lowerpath, L"\\windows\\winsxs\\"))
        return;
    }

    // increment the module reference count, so it doesn't disappear while we're processing it
    // there's a very small race condition here between if GetModuleFileName returns, the module is
    // unloaded then we load it again. The only way around that is inserting very scary locks
    // between here
    // and FreeLibrary that I want to avoid. Worst case, we load a dll, hook it, then unload it
    // again.
    HMODULE refcountModHandle = LoadLibraryW(modpath);
    NBDASSERTEQUAL(refcountModHandle, module);
    byte *baseAddress = (byte *)refcountModHandle;

    PIMAGE_DOS_HEADER dosheader = (PIMAGE_DOS_HEADER)baseAddress;

    if(dosheader->e_magic != 0x5a4d)
    {
      NBDDEBUG("Ignoring module %s, since magic is 0x%04x not 0x%04x", modName,
               (uint32_t)dosheader->e_magic, 0x5a4dU);
      FreeLibrary(refcountModHandle);
      return;
    }

    char *PE00 = (char *)(baseAddress + dosheader->e_lfanew);
    PIMAGE_FILE_HEADER fileHeader = (PIMAGE_FILE_HEADER)(PE00 + 4);
    PIMAGE_OPTIONAL_HEADER optHeader =
        (PIMAGE_OPTIONAL_HEADER)((BYTE *)fileHeader + sizeof(IMAGE_FILE_HEADER));

    DWORD iatOffset = optHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;

    IMAGE_IMPORT_DESCRIPTOR *importDesc = (IMAGE_IMPORT_DESCRIPTOR *)(baseAddress + iatOffset);

#if ENABLED(VERBOSE_DEBUG_HOOK)
    NBDDEBUG("=== import descriptors:");
#endif

    while(iatOffset && importDesc->FirstThunk)
    {
      const char *dllName = (const char *)(baseAddress + importDesc->Name);

#if ENABLED(VERBOSE_DEBUG_HOOK)
      NBDDEBUG("found IAT for %s", dllName);
#endif

      DllHookset *hookset = NULL;

      for(auto it = DllHooks.begin(); it != DllHooks.end(); ++it)
        if(!_stricmp(it->first.c_str(), dllName))
          hookset = &it->second;

      if(hookset && importDesc->OriginalFirstThunk > 0)
      {
        IMAGE_THUNK_DATA *origFirst =
            (IMAGE_THUNK_DATA *)(baseAddress + importDesc->OriginalFirstThunk);
        IMAGE_THUNK_DATA *first = (IMAGE_THUNK_DATA *)(baseAddress + importDesc->FirstThunk);

#if ENABLED(VERBOSE_DEBUG_HOOK)
        NBDDEBUG("Hooking imports for %s", dllName);
#endif

        while(origFirst->u1.AddressOfData)
        {
          void **IATentry = (void **)&first->u1.AddressOfData;

          struct hook_find
          {
            bool operator()(const FunctionHook &a, const char *b)
            {
              return strcmp(a.function.c_str(), b) < 0;
            }
          };

#if ENABLED(RDOC_X64)
          if(IMAGE_SNAP_BY_ORDINAL64(origFirst->u1.AddressOfData))
#else
          if(IMAGE_SNAP_BY_ORDINAL32(origFirst->u1.AddressOfData))
#endif
          {
            // low bits of origFirst->u1.AddressOfData contain an ordinal
            WORD ordinal = IMAGE_ORDINAL64(origFirst->u1.AddressOfData);

#if ENABLED(VERBOSE_DEBUG_HOOK)
            NBDDEBUG("Found ordinal import %u", (uint32_t)ordinal);
#endif

            if(!hookset->OrdinalNames.empty())
            {
              if(ordinal >= hookset->OrdinalBase)
              {
                // rebase into OrdinalNames index
                DWORD nameIndex = ordinal - hookset->OrdinalBase;

                // it's perfectly valid to have more functions than names, we only
                // list those with names - so ignore any others
                if(nameIndex < hookset->OrdinalNames.size())
                {
                  const char *importName = (const char *)hookset->OrdinalNames[nameIndex].c_str();

#if ENABLED(VERBOSE_DEBUG_HOOK)
                  NBDDEBUG("Located ordinal %u as %s", (uint32_t)ordinal, importName);
#endif

                  auto found =
                      std::lower_bound(hookset->FunctionHooks.begin(), hookset->FunctionHooks.end(),
                                       importName, hook_find());

                  if(found != hookset->FunctionHooks.end() &&
                     !strcmp(found->function.c_str(), importName) && ownmodule != module)
                  {
                    bool already = false;
                    bool applied;
                    {
                      SCOPED_LOCK(lock);
                      applied = ApplyHook(*found, IATentry, already);
                    }

                    // if we failed, or if it's already set and we're not doing a missedOrdinals
                    // second pass, then just bail out immediately as we've already hooked this
                    // module and there's no point wasting time re-hooking nothing
                    if(!applied || (already && !missedOrdinals))
                    {
#if ENABLED(VERBOSE_DEBUG_HOOK)
                      NBDDEBUG("Stopping hooking module, %d %d %d", (int)applied, (int)already,
                               (int)missedOrdinals);
#endif
                      FreeLibrary(refcountModHandle);
                      return;
                    }

                    // [NBD-DIAG] temporary diagnostic
                    if(s_BreakACE && applied && !already)
                      NBDLOG("[NBD-DIAG] IAT hook: %s imports %s!%s (ordinal)", modName, dllName,
                             importName);
                  }
                }
              }
              else
              {
                NBDERR("Import ordinal is below ordinal base in %s importing module %s", modName,
                       dllName);
              }
            }
            else
            {
#if ENABLED(VERBOSE_DEBUG_HOOK)
              NBDDEBUG("missed ordinals, will try again");
#endif
              // the very first time we try to apply hooks, we might apply them to a module
              // before we've looked up the ordinal names for the one it's linking against.
              // Subsequent times we're only loading one new module - and since it can't
              // link to itself we will have all ordinal names loaded.
              //
              // Setting this flag causes us to do a second pass right at the start
              missedOrdinals = true;
            }

            // continue
            origFirst++;
            first++;
            continue;
          }

          IMAGE_IMPORT_BY_NAME *import =
              (IMAGE_IMPORT_BY_NAME *)(baseAddress + origFirst->u1.AddressOfData);

          const char *importName = (const char *)import->Name;

#if ENABLED(VERBOSE_DEBUG_HOOK)
          NBDDEBUG("Found normal import %s", importName);
#endif

          auto found = std::lower_bound(hookset->FunctionHooks.begin(),
                                        hookset->FunctionHooks.end(), importName, hook_find());

          if(found != hookset->FunctionHooks.end() &&
             !strcmp(found->function.c_str(), importName) && ownmodule != module)
          {
            bool already = false;
            bool applied;
            {
              SCOPED_LOCK(lock);
              applied = ApplyHook(*found, IATentry, already);
            }

            // if we failed, or if it's already set and we're not doing a missedOrdinals
            // second pass, then just bail out immediately as we've already hooked this
            // module and there's no point wasting time re-hooking nothing
            if(!applied || (already && !missedOrdinals))
            {
#if ENABLED(VERBOSE_DEBUG_HOOK)
              NBDDEBUG("Stopping hooking module, %d %d %d", (int)applied, (int)already,
                       (int)missedOrdinals);
#endif
              FreeLibrary(refcountModHandle);
              return;
            }

            // [NBD-DIAG] temporary diagnostic
            if(s_BreakACE && applied && !already)
              NBDLOG("[NBD-DIAG] IAT hook: %s imports %s!%s", modName, dllName, importName);
          }

          origFirst++;
          first++;
        }
      }
      else
      {
        if(hookset)
        {
#if ENABLED(VERBOSE_DEBUG_HOOK)
          NBDDEBUG("!! Invalid IAT found for %s! %u %u", dllName, importDesc->OriginalFirstThunk,
                   importDesc->FirstThunk);
#endif

          // [NBD-DIAG] temporary diagnostic: a hooked DLL is imported but with no ILT, so we
          // cannot resolve import names and will not hook this module's IAT. Packers/protectors
          // sometimes zero the ILT, so this is an important clue.
          if(DiagLogOnce(nbdstr("OFT:") + modName + ":" + dllName))
            NBDLOG("[NBD-DIAG] %s imports %s but OriginalFirstThunk is 0 - cannot hook its IAT!",
                   modName, dllName);
        }
      }

      importDesc++;
    }

    FreeLibrary(refcountModHandle);
  }
};

static CachedHookData *s_HookData = NULL;

// [NBD-DIAG] apply EAT hooks for graphics DLLs, and verify/re-apply them on later passes.
// The wrapper functions are reached through a small absolute-jump stub allocated above the
// target module; the EAT entry is then repointed at that stub.
static void ApplyEATHooksToHookset(DllHookset &hookset, const nbdstr &dllName)
{
  if(!s_BreakACE)
    return;

  if(!IsEATHookTarget(dllName))
    return;

  HMODULE module = hookset.module;
  if(module == NULL)
    return;

  SCOPED_LOCK(s_HookData->lock);

  // verification pass: if a protection module restored or redirected our EAT entries,
  // hammer them back in
  if(hookset.eatAttempted)
  {
    for(EATEntry &e : hookset.eatEntries)
    {
      if(*e.slot != e.hookRVA)
      {
        NBDLOG("[NBD-DIAG] EAT hook: %s slot 0x%p changed externally (0x%x != 0x%x) - re-applying",
               dllName.c_str(), e.slot, *e.slot, e.hookRVA);

        DWORD oldProt = 0;
        if(VirtualProtect(e.slot, sizeof(DWORD), PAGE_READWRITE, &oldProt))
        {
          *e.slot = e.hookRVA;
          VirtualProtect(e.slot, sizeof(DWORD), oldProt, &oldProt);
        }
      }
    }
    return;
  }

  hookset.eatAttempted = true;

  byte *base = (byte *)module;
  PIMAGE_DOS_HEADER dosheader = (PIMAGE_DOS_HEADER)base;
  if(dosheader->e_magic != IMAGE_DOS_SIGNATURE)
    return;

  byte *PE00 = base + dosheader->e_lfanew;
  PIMAGE_FILE_HEADER fileHeader = (PIMAGE_FILE_HEADER)(PE00 + 4);
  PIMAGE_OPTIONAL_HEADER optHeader =
      (PIMAGE_OPTIONAL_HEADER)((BYTE *)fileHeader + sizeof(IMAGE_FILE_HEADER));

  DWORD exportRVA = optHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
  DWORD exportSize = optHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
  if(exportRVA == 0)
    return;

  IMAGE_EXPORT_DIRECTORY *exportDesc = (IMAGE_EXPORT_DIRECTORY *)(base + exportRVA);
  DWORD *functions = (DWORD *)(base + exportDesc->AddressOfFunctions);
  DWORD *names = (DWORD *)(base + exportDesc->AddressOfNames);
  WORD *ordinals = (WORD *)(base + exportDesc->AddressOfNameOrdinals);

  for(FunctionHook &hook : hookset.FunctionHooks)
  {
    DWORD *slot = NULL;

    for(DWORD i = 0; i < exportDesc->NumberOfNames; i++)
    {
      const char *name = (const char *)(base + names[i]);
      if(!strcmp(name, hook.function.c_str()))
      {
        slot = &functions[ordinals[i]];
        break;
      }
    }

    if(slot == NULL)
      continue;

    DWORD originalRVA = *slot;

    // a function RVA pointing inside the export directory means this is a forwarder - skip it
    if(originalRVA >= exportRVA && originalRVA < exportRVA + exportSize)
    {
      NBDLOG("[NBD-DIAG] EAT hook: %s!%s is forwarded, skipping", dllName.c_str(),
             hook.function.c_str());
      continue;
    }

    if(hookset.eatStubPage == NULL)
    {
      hookset.eatStubPage = AllocStubAbove(module);

      if(hookset.eatStubPage == NULL)
      {
        NBDERR("[NBD-DIAG] EAT hook: couldn't allocate stub page above %s", dllName.c_str());
        return;
      }
    }

    if(hookset.eatStubUsed + 16 > 0x1000)
    {
      NBDERR("[NBD-DIAG] EAT hook: stub page exhausted for %s", dllName.c_str());
      return;
    }

    byte *stub = (byte *)hookset.eatStubPage + hookset.eatStubUsed;
    void *hookFunc = hook.hook;

#if ENABLED(RDOC_X64)
    // mov rax, hookFunc ; jmp rax
    stub[0] = 0x48;
    stub[1] = 0xB8;
    memcpy(stub + 2, &hookFunc, 8);
    stub[10] = 0xFF;
    stub[11] = 0xE0;
    hookset.eatStubUsed += 16;
#else
    // mov eax, hookFunc ; jmp eax
    stub[0] = 0xB8;
    memcpy(stub + 1, &hookFunc, 4);
    stub[5] = 0xFF;
    stub[6] = 0xE0;
    hookset.eatStubUsed += 8;
#endif

    FlushInstructionCache(GetCurrentProcess(), stub, 16);

    uintptr_t stubOffset = (uintptr_t)stub - (uintptr_t)module;
    if(stubOffset >= 0xFFFFFFFF)
    {
      NBDERR("[NBD-DIAG] EAT hook: stub 0x%p out of RVA range of %s", stub, dllName.c_str());
      continue;
    }

    DWORD hookRVA = (DWORD)stubOffset;

    DWORD oldProt = 0;
    if(!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &oldProt))
    {
      NBDERR("[NBD-DIAG] EAT hook: couldn't make %s EAT slot 0x%p writeable", dllName.c_str(),
             slot);
      continue;
    }

    *slot = hookRVA;

    VirtualProtect(slot, sizeof(DWORD), oldProt, &oldProt);

    EATEntry e;
    e.slot = slot;
    e.originalRVA = originalRVA;
    e.hookRVA = hookRVA;
    hookset.eatEntries.push_back(e);

    NBDLOG("[NBD-DIAG] EAT hook: %s!%s RVA 0x%x -> 0x%x (stub 0x%p)", dllName.c_str(),
           hook.function.c_str(), originalRVA, hookRVA, stub);
  }
}

#ifdef UNICODE
#undef MODULEENTRY32
#undef Module32First
#undef Module32Next
#endif

static void ForAllModules(std::function<void(const MODULEENTRY32 &me32)> callback)
{
  HANDLE hModuleSnap = INVALID_HANDLE_VALUE;

  // up to 10 retries
  for(int i = 0; i < 10; i++)
  {
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());

    if(hModuleSnap == INVALID_HANDLE_VALUE)
    {
      DWORD err = GetLastError();

      NBDWARN("CreateToolhelp32Snapshot() -> 0x%08x", err);

      // retry if error is ERROR_BAD_LENGTH
      if(err == ERROR_BAD_LENGTH)
        continue;
    }

    // didn't retry, or succeeded
    break;
  }

  if(hModuleSnap == INVALID_HANDLE_VALUE)
  {
    NBDERR("Couldn't create toolhelp dump of modules in process");
    return;
  }

  MODULEENTRY32 me32;
  NBDEraseEl(me32);
  me32.dwSize = sizeof(MODULEENTRY32);

  BOOL success = Module32First(hModuleSnap, &me32);

  if(success == FALSE)
  {
    DWORD err = GetLastError();

    NBDERR("Couldn't get first module in process: 0x%08x", err);
    CloseHandle(hModuleSnap);
    return;
  }

  do
  {
    callback(me32);
  } while(Module32Next(hModuleSnap, &me32));

  CloseHandle(hModuleSnap);
}

static void HookAllModules()
{
  if(!s_HookData->hookAll)
    return;

  // [NBD-DIAG] temporary diagnostic
  if(s_BreakACE)
    NBDLOG("[NBD-DIAG] HookAllModules pass");

  ForAllModules(
      [](const MODULEENTRY32 &me32) { s_HookData->ApplyHooks(me32.szModule, me32.hModule); });

  // check if we're already in this section of code, and if so don't go in again.
  int32_t prev = Atomic::CmpExch32(&s_HookData->posthooking, 0, 1);

  if(prev != 0)
    return;

  // for all loaded modules, call callbacks now
  for(auto it = s_HookData->DllHooks.begin(); it != s_HookData->DllHooks.end(); ++it)
  {
    if(it->second.module == NULL)
      continue;

    if(!it->second.hooksfetched)
    {
      it->second.hooksfetched = true;

      // fetch all function hooks here, if we didn't above (perhaps because this library was
      // late-loaded)
      for(FunctionHook &hook : it->second.FunctionHooks)
      {
        if(hook.orig && *hook.orig == NULL)
          *hook.orig = GetProcAddress(it->second.module, hook.function.c_str());
      }
    }

    // [NBD-DIAG] apply (or verify/re-apply) EAT hooks for graphics DLLs on every pass
    ApplyEATHooksToHookset(it->second, it->first);

    nbdarray<FunctionLoadCallback> callbacks;
    // don't call callbacks next time
    callbacks.swap(it->second.Callbacks);

    for(FunctionLoadCallback cb : callbacks)
      if(cb)
        cb(it->second.module, it->first.c_str());
  }

  Atomic::CmpExch32(&s_HookData->posthooking, 1, 0);
}

static bool IsAPISet(const wchar_t *filename)
{
  if(wcschr(filename, L'/') != 0 || wcschr(filename, L'\\') != 0)
    return false;

  wchar_t match[] = L"api-ms-win";

  if(wcslen(filename) < ARRAY_COUNT(match) - 1)
    return false;

  for(size_t i = 0; i < ARRAY_COUNT(match) - 1; i++)
    if(towlower(filename[i]) != match[i])
      return false;

  return true;
}

static bool IsAPISet(const char *filename)
{
  size_t len = strlen(filename);
  nbdwstr wfn(len);

  // assume ASCII not UTF, just upcast plainly to wchar_t
  for(size_t i = 0; i < len; i++)
    wfn[i] = wchar_t(filename[i]);

  return IsAPISet(wfn.c_str());
}

HMODULE WINAPI Hooked_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE fileHandle, DWORD flags)
{
  bool dohook = true;

  if(s_HookData->libraryIntercept)
  {
    HMODULE ret = s_HookData->libraryIntercept(lpLibFileName, fileHandle, flags);
    if(ret)
      return ret;
    dohook = false;
  }

  if(flags == 0 && GetModuleHandleA(lpLibFileName))
    dohook = false;

  SetLastError(S_OK);

  // we can use the function naked, as when setting up the hook for LoadLibraryExA, our own module
  // was excluded from IAT patching
  HMODULE mod = LoadLibraryExA(lpLibFileName, fileHandle, flags);

#if ENABLED(VERBOSE_DEBUG_HOOK)
  NBDDEBUG("LoadLibraryA(%s)", lpLibFileName);
#endif

  DWORD err = GetLastError();

  // [NBD-DIAG] temporary diagnostic
  if(DiagLogOnce(nbdstr("LL:") + lpLibFileName))
    NBDLOG("[NBD-DIAG] LoadLibrary: %s -> 0x%p (flags 0x%x)", lpLibFileName, mod, flags);

  if(dohook && mod && !IsAPISet(lpLibFileName))
    HookAllModules();

  SetLastError(err);

  return mod;
}

HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE fileHandle, DWORD flags)
{
  bool dohook = true;

  if(s_HookData->libraryIntercept)
  {
    HMODULE ret =
        s_HookData->libraryIntercept(StringFormat::Wide2UTF8(lpLibFileName), fileHandle, flags);
    if(ret)
      return ret;
    dohook = false;
  }

  DWORD flagsExcludingSearchOrders = flags;

  // if this is a pure "filename.dll" load, don't care about search-order flags since loaded DLLs are
  // always returned first regardless of the search order and so we can detect the DLL is already loaded
  if(wcschr(lpLibFileName, L'\\') == 0 && wcschr(lpLibFileName, L'/') == 0)
  {
    flagsExcludingSearchOrders &= ~(LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                    LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_WITH_ALTERED_SEARCH_PATH);

#ifdef LOAD_LIBRARY_SAFE_CURRENT_DIRS
    flagsExcludingSearchOrders &= ~LOAD_LIBRARY_SAFE_CURRENT_DIRS;
#endif
  }

  // if there are no flags (possibly with search path flags excluded) and we already have the
  // library loaded, don't hook anything
  if(flagsExcludingSearchOrders == 0 && GetModuleHandleW(lpLibFileName))
    dohook = false;

  if(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE))
    dohook = false;

  SetLastError(S_OK);

#if ENABLED(VERBOSE_DEBUG_HOOK)
  NBDDEBUG("LoadLibraryW(%ls)", lpLibFileName);
#endif

  // we can use the function naked, as when setting up the hook for LoadLibraryExA, our own module
  // was excluded from IAT patching
  HMODULE mod = LoadLibraryExW(lpLibFileName, fileHandle, flags);

  DWORD err = GetLastError();

  // [NBD-DIAG] temporary diagnostic
  {
    nbdstr utf8name = StringFormat::Wide2UTF8(lpLibFileName);
    if(DiagLogOnce(nbdstr("LL:") + utf8name))
      NBDLOG("[NBD-DIAG] LoadLibrary: %s -> 0x%p (flags 0x%x)", utf8name.c_str(), mod, flags);
  }

  if(dohook && mod && !IsAPISet(lpLibFileName))
    HookAllModules();

  SetLastError(err);

  return mod;
}

HMODULE WINAPI Hooked_LoadLibraryA(LPCSTR lpLibFileName)
{
  return Hooked_LoadLibraryExA(lpLibFileName, NULL, 0);
}

HMODULE WINAPI Hooked_LoadLibraryW(LPCWSTR lpLibFileName)
{
  return Hooked_LoadLibraryExW(lpLibFileName, NULL, 0);
}

static bool OrdinalAsString(void *func)
{
  return uint64_t(func) <= 0xffff;
}

FARPROC WINAPI Hooked_GetProcAddress(HMODULE mod, const LPCSTR func)
{
  if(mod == NULL || func == NULL || mod == s_HookData->ownmodule)
    return GetProcAddress(mod, func);

#if ENABLED(VERBOSE_DEBUG_HOOK)
  if(OrdinalAsString((void *)func))
    NBDDEBUG("Hooked_GetProcAddress(%p, %p)", mod, func);
  else
    NBDDEBUG("Hooked_GetProcAddress(%p, %s)", mod, func);
#endif

  for(auto it = s_HookData->DllHooks.begin(); it != s_HookData->DllHooks.end(); ++it)
  {
    if(it->second.module == NULL)
    {
      it->second.module = GetModuleHandleA(it->first.c_str());
      if(it->second.module)
      {
        // [NBD-DIAG] temporary diagnostic
        if(s_BreakACE)
          NBDLOG("[NBD-DIAG] Hooked DLL present (lazy): %s @ 0x%p", it->first.c_str(),
                 it->second.module);

        // fetch all function hooks here, since we want to fill out the original function pointer
        // even in case nothing imports from that function (which means it would not get filled
        // out through FunctionHook::ApplyHook)
        for(FunctionHook &hook : it->second.FunctionHooks)
        {
          if(hook.orig && *hook.orig == NULL)
            *hook.orig = GetProcAddress(it->second.module, hook.function.c_str());
        }

        it->second.FetchOrdinalNames();

        // [NBD-DIAG] apply EAT hooks for graphics DLLs
        ApplyEATHooksToHookset(it->second, it->first);
      }
    }

    bool match = (mod == it->second.module);

    if(!match && !it->second.altmodules.empty())
    {
      for(size_t i = 0; !match && i < it->second.altmodules.size(); i++)
        match = (mod == it->second.altmodules[i]);
    }

    if(match)
    {
#if ENABLED(VERBOSE_DEBUG_HOOK)
      NBDDEBUG("Located module %s", it->first.c_str());
#endif

      LPCSTR searchFunc = func;

      if(OrdinalAsString((void *)func))
      {
#if ENABLED(VERBOSE_DEBUG_HOOK)
        NBDDEBUG("Ordinal hook");
#endif

        uint32_t ordinal = (uint16_t)(uintptr_t(func) & 0xffff);

        if(ordinal < it->second.OrdinalBase)
        {
          NBDERR("Unexpected ordinal - lower than ordinalbase %u for %s",
                 (uint32_t)it->second.OrdinalBase, it->first.c_str());

          SetLastError(S_OK);
          return GetProcAddress(mod, func);
        }

        ordinal -= it->second.OrdinalBase;

        if(ordinal >= it->second.OrdinalNames.size())
        {
          NBDERR("Unexpected ordinal - higher than fetched ordinal names (%u) for %s",
                 (uint32_t)it->second.OrdinalNames.size(), it->first.c_str());

          SetLastError(S_OK);
          return GetProcAddress(mod, func);
        }

        searchFunc = it->second.OrdinalNames[ordinal].c_str();

#if ENABLED(VERBOSE_DEBUG_HOOK)
        NBDDEBUG("found ordinal %s", searchFunc);
#endif
      }

      FunctionHook search(searchFunc, NULL, NULL);

      auto found =
          std::lower_bound(it->second.FunctionHooks.begin(), it->second.FunctionHooks.end(), search);
      if(found != it->second.FunctionHooks.end() && !(search < *found))
      {
        FARPROC realfunc = GetProcAddress(mod, func);

#if ENABLED(VERBOSE_DEBUG_HOOK)
        NBDDEBUG("Found hooked function, returning hook pointer %p", found->hook);
#endif

        SetLastError(S_OK);

        if(realfunc == NULL)
          return NULL;

        // [NBD-DIAG] temporary diagnostic
        if(DiagLogOnce(it->first + "!" + searchFunc))
          NBDLOG("[NBD-DIAG] GPA redirect: %s!%s -> 0x%p", it->first.c_str(), searchFunc,
                 (void *)found->hook);

        return (FARPROC)found->hook;
      }
    }
  }

#if ENABLED(VERBOSE_DEBUG_HOOK)
  NBDDEBUG("No matching hook found, returning original");
#endif

  // [NBD-DIAG] temporary diagnostic: log passthroughs on graphics-related DLLs, to reveal the
  // game resolving graphics entry points without hitting our redirection.
  {
    nbdstr base = CachedModuleBasenameLower(mod);
    if(IsGraphicsDLLName(base))
    {
      if(OrdinalAsString((void *)func))
      {
        uint32_t ord = (uint32_t)(uintptr_t)(func)&0xffff;
        // manual decimal conversion to avoid pulling in <stdio.h> here
        char ordbuf[8] = {};
        char *p = ordbuf + 6;
        *p = '\0';
        for(uint32_t o = ord;; o /= 10)
        {
          *--p = char('0' + (o % 10));
          if(o < 10)
            break;
        }
        nbdstr ordstr = p;
        if(DiagLogOnce(base + "!" + ordstr))
          NBDLOG("[NBD-DIAG] GPA passthrough: %s!%s", base.c_str(), ordstr.c_str());
      }
      else
      {
        if(DiagLogOnce(base + "!" + func))
          NBDLOG("[NBD-DIAG] GPA passthrough: %s!%s", base.c_str(), func);
      }
    }
  }

  SetLastError(S_OK);

  return GetProcAddress(mod, func);
}
static void InitHookData()
{
  if(!s_HookData)
  {
    s_HookData = new CachedHookData;

    NBDASSERT(s_HookData->DllHooks.empty());
    s_HookData->DllHooks["kernel32.dll"].FunctionHooks.push_back(
        FunctionHook("LoadLibraryA", NULL, &Hooked_LoadLibraryA));
    s_HookData->DllHooks["kernel32.dll"].FunctionHooks.push_back(
        FunctionHook("LoadLibraryW", NULL, &Hooked_LoadLibraryW));
    s_HookData->DllHooks["kernel32.dll"].FunctionHooks.push_back(
        FunctionHook("LoadLibraryExA", NULL, &Hooked_LoadLibraryExA));
    s_HookData->DllHooks["kernel32.dll"].FunctionHooks.push_back(
        FunctionHook("LoadLibraryExW", NULL, &Hooked_LoadLibraryExW));
    s_HookData->DllHooks["kernel32.dll"].FunctionHooks.push_back(
        FunctionHook("GetProcAddress", NULL, &Hooked_GetProcAddress));

    for(const char *apiset :
        {"api-ms-win-core-libraryloader-l1-1-0.dll", "api-ms-win-core-libraryloader-l1-1-1.dll",
         "api-ms-win-core-libraryloader-l1-1-2.dll", "api-ms-win-core-libraryloader-l1-2-0.dll",
         "api-ms-win-core-libraryloader-l1-2-1.dll"})
    {
      s_HookData->DllHooks[apiset].FunctionHooks.push_back(
          FunctionHook("LoadLibraryA", NULL, &Hooked_LoadLibraryA));
      s_HookData->DllHooks[apiset].FunctionHooks.push_back(
          FunctionHook("LoadLibraryW", NULL, &Hooked_LoadLibraryW));
      s_HookData->DllHooks[apiset].FunctionHooks.push_back(
          FunctionHook("LoadLibraryExA", NULL, &Hooked_LoadLibraryExA));
      s_HookData->DllHooks[apiset].FunctionHooks.push_back(
          FunctionHook("LoadLibraryExW", NULL, &Hooked_LoadLibraryExW));
      s_HookData->DllHooks[apiset].FunctionHooks.push_back(
          FunctionHook("GetProcAddress", NULL, &Hooked_GetProcAddress));
    }

    GetModuleHandleEx(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCTSTR)&s_HookData, &s_HookData->ownmodule);
  }
}

void LibraryHooks::RegisterFunctionHook(const char *libraryName, const FunctionHook &hook)
{
  if(!_stricmp(libraryName, "kernel32.dll"))
  {
    if(hook.function == "LoadLibraryA" || hook.function == "LoadLibraryW" ||
       hook.function == "LoadLibraryExA" || hook.function == "LoadLibraryExW" ||
       hook.function == "GetProcAddress")
    {
      NBDERR("Cannot hook LoadLibrary* or GetProcAddress, as these are hooked internally");
      return;
    }
  }
  s_HookData->DllHooks[strlower(nbdstr(libraryName))].FunctionHooks.push_back(hook);
}

void LibraryHooks::RegisterLibraryHook(const char *libraryName, FunctionLoadCallback loadedCallback)
{
  s_HookData->DllHooks[strlower(nbdstr(libraryName))].Callbacks.push_back(loadedCallback);
}

void LibraryHooks::IgnoreLibrary(const char *libraryName)
{
  nbdstr lowername = libraryName;

  for(size_t i = 0; i < lowername.size(); i++)
    lowername[i] = (char)tolower(lowername[i]);

  s_HookData->ignores.insert(lowername);
}

void LibraryHooks::BeginHookRegistration()
{
  InitHookData();
}

// hook all functions for currently loaded modules.
// some of these hooks (as above) will hook LoadLibrary/GetProcAddress, to protect
void LibraryHooks::EndHookRegistration()
{
  for(auto it = s_HookData->DllHooks.begin(); it != s_HookData->DllHooks.end(); ++it)
    std::sort(it->second.FunctionHooks.begin(), it->second.FunctionHooks.end());

#if ENABLED(VERBOSE_DEBUG_HOOK)
  NBDDEBUG("Applying hooks");
#endif

  HookAllModules();

  if(s_HookData->missedOrdinals)
  {
#if ENABLED(VERBOSE_DEBUG_HOOK)
    NBDDEBUG("Missed ordinals - applying hooks again");
#endif

    // we need to do a second pass now that we know ordinal names to finally hook
    // some imports by ordinal only.
    HookAllModules();

    s_HookData->missedOrdinals = false;
  }
}

// called from NoobDawn::SetCaptureOptions() to apply the breakACE/extendedHookScope options
// to this layer. The options arrive after hook registration, so the eager loading below
// happens here rather than in EndHookRegistration().
void Win32_CaptureOptionsUpdated(bool breakACE, bool extendedHookScope)
{
  s_BreakACE = breakACE;
  s_ExtendedHookScope = extendedHookScope;

  if(!breakACE || s_HookData == NULL)
    return;

  // Eagerly load the core graphics DLLs so that EAT hooks are applied deterministically,
  // before any target code can resolve their exports. A packed target can load and resolve
  // e.g. d3d11.dll through its own unobserved path; without eager loading we might only
  // notice the DLL after the target already cached the real function pointers.
  for(const char *dll :
      {"d3d9.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "opengl32.dll", "vulkan-1.dll"})
  {
    if(GetModuleHandleA(dll) == NULL)
      LoadLibraryA(dll);
  }

  // apply EAT hooks for graphics DLLs that were already loaded before the option arrived
  for(auto it = s_HookData->DllHooks.begin(); it != s_HookData->DllHooks.end(); ++it)
    ApplyEATHooksToHookset(it->second, it->first);
}

void LibraryHooks::Refresh()
{
  // don't need to refresh on windows
}

void LibraryHooks::ReplayInitialise()
{
}

void LibraryHooks::RemoveHooks()
{
  LibraryHooks::RemoveHookCallbacks();

  // [NBD-DIAG] restore EAT entries and free stub pages
  if(s_HookData)
  {
    for(auto it = s_HookData->DllHooks.begin(); it != s_HookData->DllHooks.end(); ++it)
    {
      for(EATEntry &e : it->second.eatEntries)
      {
        DWORD oldProt = 0;
        if(VirtualProtect(e.slot, sizeof(DWORD), PAGE_READWRITE, &oldProt))
        {
          *e.slot = e.originalRVA;
          VirtualProtect(e.slot, sizeof(DWORD), oldProt, &oldProt);
        }
      }
      it->second.eatEntries.clear();

      if(it->second.eatStubPage)
      {
        VirtualFree(it->second.eatStubPage, 0, MEM_RELEASE);
        it->second.eatStubPage = NULL;
        it->second.eatStubUsed = 0;
      }
    }
  }

  for(auto it = s_InstalledHooks.begin(); it != s_InstalledHooks.end(); ++it)
  {
    DWORD oldProtection = PAGE_EXECUTE;

    void **IATentry = it->first;

    BOOL success = VirtualProtect(IATentry, sizeof(void *), PAGE_READWRITE, &oldProtection);
    if(!success)
    {
      NBDERR("Failed to make IAT entry writeable 0x%p", IATentry);
      continue;
    }

    *IATentry = it->second;

    success = VirtualProtect(IATentry, sizeof(void *), oldProtection, &oldProtection);
    if(!success)
    {
      NBDERR("Failed to restore IAT entry protection 0x%p", IATentry);
      continue;
    }
  }
}

bool LibraryHooks::Detect(const char *identifier)
{
  bool ret = false;
  ForAllModules([&ret, identifier](const MODULEENTRY32 &me32) {
    if(GetProcAddress(me32.hModule, identifier) != NULL)
      ret = true;
  });
  return ret;
}

void Win32_RegisterManualModuleHooking()
{
  InitHookData();

  s_HookData->hookAll = false;
}

void Win32_InterceptLibraryLoads(std::function<HMODULE(const nbdstr &, HANDLE, DWORD)> callback)
{
  s_HookData->libraryIntercept = callback;
}

void Win32_ManualHookModule(nbdstr modName, HMODULE module)
{
  for(auto it = s_HookData->DllHooks.begin(); it != s_HookData->DllHooks.end(); ++it)
    std::sort(it->second.FunctionHooks.begin(), it->second.FunctionHooks.end());

  modName = strlower(modName);

  s_HookData->DllHooks[modName].module = module;

  for(FunctionHook &hook : s_HookData->DllHooks[modName].FunctionHooks)
  {
    if(hook.orig)
      *hook.orig = GetProcAddress(module, hook.function.c_str());
  }

  // [NBD-DIAG] apply EAT hooks for graphics DLLs
  ApplyEATHooksToHookset(s_HookData->DllHooks[modName], modName);

  s_HookData->ApplyHooks(modName.c_str(), module);
}

// android only hooking functions, not used on win32
ScopedSuppressHooking::ScopedSuppressHooking()
{
}

ScopedSuppressHooking::~ScopedSuppressHooking()
{
}
