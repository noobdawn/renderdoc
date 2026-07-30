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

#include "android/android.h"
#include "api/replay/noobdawn_replay.h"
#include "api/replay/version.h"
#include "common/common.h"
#include "common/formatting.h"
#include "common/threading.h"
#include "core/core.h"
#include "maths/camera.h"
#include "maths/formatpacking.h"
#include "miniz/miniz.h"
#include "replay/replay_driver.h"
#include "strings/string_utils.h"
#include "superluminal/superluminal.h"

Threading::CriticalSection detailStringLock;
nbdarray<const nbdstr *> detailStrings;

#if ENABLED(ENABLE_UNIT_TESTS)
bool RDResult::testErrorExpected = false;
#endif

RDResult::operator ResultDetails() const
{
  NBDCOMPILE_ASSERT(ResultCode(0) == ResultCode::Succeeded,
                    "ResultCode 0 value should be succeeded");

  ResultDetails ret;
  ret.code = code;
  ret.internal_msg = NULL;
  if(!message.empty())
  {
    SCOPED_LOCK(detailStringLock);
    ret.internal_msg = new nbdstr(ToStr(code) + ": " + message);
    detailStrings.push_back(ret.internal_msg);
  }

  return ret;
}

// these entry points are for the replay/analysis side - not for the application.

extern "C" NOOBDAWN_API uint32_t NOOBDAWN_CC NOOBDAWN_NumVerticesPerPrimitive(Topology topology)
{
  // strips/loops/fans have the same number of indices for a single primitive
  // as their list friends
  switch(topology)
  {
    default:
    case Topology::Unknown: break;
    case Topology::PointList: return 1;
    case Topology::LineList:
    case Topology::LineStrip:
    case Topology::LineLoop: return 2;
    case Topology::TriangleList:
    case Topology::TriangleStrip:
    case Topology::TriangleFan: return 3;
    case Topology::LineList_Adj:
    case Topology::LineStrip_Adj: return 4;
    case Topology::TriangleList_Adj:
    case Topology::TriangleStrip_Adj: return 6;
    case Topology::PatchList_1CPs:
    case Topology::PatchList_2CPs:
    case Topology::PatchList_3CPs:
    case Topology::PatchList_4CPs:
    case Topology::PatchList_5CPs:
    case Topology::PatchList_6CPs:
    case Topology::PatchList_7CPs:
    case Topology::PatchList_8CPs:
    case Topology::PatchList_9CPs:
    case Topology::PatchList_10CPs:
    case Topology::PatchList_11CPs:
    case Topology::PatchList_12CPs:
    case Topology::PatchList_13CPs:
    case Topology::PatchList_14CPs:
    case Topology::PatchList_15CPs:
    case Topology::PatchList_16CPs:
    case Topology::PatchList_17CPs:
    case Topology::PatchList_18CPs:
    case Topology::PatchList_19CPs:
    case Topology::PatchList_20CPs:
    case Topology::PatchList_21CPs:
    case Topology::PatchList_22CPs:
    case Topology::PatchList_23CPs:
    case Topology::PatchList_24CPs:
    case Topology::PatchList_25CPs:
    case Topology::PatchList_26CPs:
    case Topology::PatchList_27CPs:
    case Topology::PatchList_28CPs:
    case Topology::PatchList_29CPs:
    case Topology::PatchList_30CPs:
    case Topology::PatchList_31CPs:
    case Topology::PatchList_32CPs: return PatchList_Count(topology);
  }

  return 0;
}

extern "C" NOOBDAWN_API uint32_t NOOBDAWN_CC NOOBDAWN_VertexOffset(Topology topology,
                                                                      uint32_t primitive)
{
  // strips/loops/fans have the same number of indices for a single primitive
  // as their list friends
  switch(topology)
  {
    default:
    case Topology::Unknown:
    case Topology::PointList:
    case Topology::LineList:
    case Topology::TriangleList:
    case Topology::LineList_Adj:
    case Topology::TriangleList_Adj:
    case Topology::PatchList_1CPs:
    case Topology::PatchList_2CPs:
    case Topology::PatchList_3CPs:
    case Topology::PatchList_4CPs:
    case Topology::PatchList_5CPs:
    case Topology::PatchList_6CPs:
    case Topology::PatchList_7CPs:
    case Topology::PatchList_8CPs:
    case Topology::PatchList_9CPs:
    case Topology::PatchList_10CPs:
    case Topology::PatchList_11CPs:
    case Topology::PatchList_12CPs:
    case Topology::PatchList_13CPs:
    case Topology::PatchList_14CPs:
    case Topology::PatchList_15CPs:
    case Topology::PatchList_16CPs:
    case Topology::PatchList_17CPs:
    case Topology::PatchList_18CPs:
    case Topology::PatchList_19CPs:
    case Topology::PatchList_20CPs:
    case Topology::PatchList_21CPs:
    case Topology::PatchList_22CPs:
    case Topology::PatchList_23CPs:
    case Topology::PatchList_24CPs:
    case Topology::PatchList_25CPs:
    case Topology::PatchList_26CPs:
    case Topology::PatchList_27CPs:
    case Topology::PatchList_28CPs:
    case Topology::PatchList_29CPs:
    case Topology::PatchList_30CPs:
    case Topology::PatchList_31CPs:
    case Topology::PatchList_32CPs:
      // for all lists, it's just primitive * Topology_NumVerticesPerPrimitive(topology)
      break;
    case Topology::LineStrip:
    case Topology::LineLoop:
    case Topology::TriangleStrip:
    case Topology::LineStrip_Adj:
    case Topology::TriangleFan:
      // for strips, each new vertex creates a new primitive
      return primitive;
    case Topology::TriangleStrip_Adj:
      // triangle strip with adjacency is a special case as every other
      // vert is purely for adjacency so it's doubled
      return primitive * 2;
  }

  return primitive * NOOBDAWN_NumVerticesPerPrimitive(topology);
}

extern "C" NOOBDAWN_API float NOOBDAWN_CC NOOBDAWN_HalfToFloat(uint16_t half)
{
  return ConvertFromHalf(half);
}

extern "C" NOOBDAWN_API uint16_t NOOBDAWN_CC NOOBDAWN_FloatToHalf(float f)
{
  return ConvertToHalf(f);
}

extern "C" NOOBDAWN_API ICamera *NOOBDAWN_CC NOOBDAWN_InitCamera(CameraType type)
{
  return new Camera(type);
}

extern "C" NOOBDAWN_API const char *NOOBDAWN_CC NOOBDAWN_GetVersionString()
{
  return MAJOR_MINOR_VERSION_STRING;
}

extern "C" NOOBDAWN_API bool NOOBDAWN_CC NOOBDAWN_IsReleaseBuild()
{
#if ENABLED(RDOC_RELEASE)
  return true;
#else
  return false;
#endif
}

extern "C" NOOBDAWN_API const char *NOOBDAWN_CC NOOBDAWN_GetCommitHash()
{
  return GitVersionHash;
}

extern "C" NOOBDAWN_API DriverInformation NOOBDAWN_CC NOOBDAWN_GetDriverInformation(GraphicsAPI api)
{
  return NoobDawn::Inst().GetDriverInformation(api);
}

extern "C" NOOBDAWN_API uint64_t NOOBDAWN_CC NOOBDAWN_GetCurrentProcessMemoryUsage()
{
  return Process::GetMemoryUsage();
}

extern "C" NOOBDAWN_API const SDObject *NOOBDAWN_CC NOOBDAWN_GetConfigSetting(const nbdstr &name)
{
  return NoobDawn::Inst().GetConfigSetting(name);
}

extern "C" NOOBDAWN_API SDObject *NOOBDAWN_CC NOOBDAWN_SetConfigSetting(const nbdstr &name)
{
  return NoobDawn::Inst().SetConfigSetting(name);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_SaveConfigSettings()
{
  return NoobDawn::Inst().SaveConfigSettings();
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_SetColors(FloatVector darkChecker,
                                                               FloatVector lightChecker,
                                                               bool darkTheme)
{
  NoobDawn::Inst().SetDarkCheckerboardColor(darkChecker);
  NoobDawn::Inst().SetLightCheckerboardColor(lightChecker);
  NoobDawn::Inst().SetDarkTheme(darkTheme);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_SetDebugLogFile(const nbdstr &log)
{
  if(!log.empty())
  {
    NBDLOGFILE(log.c_str());

    // need to recreate the crash handler to propagate the new log filename.
    NoobDawn::Inst().RecreateCrashHandler();
  }
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_LogMessage(LogType type, const nbdstr &project,
                                                                const nbdstr &file,
                                                                unsigned int line, const nbdstr &text)
{
  nbdlog_direct(FILL_AUTO_VALUE, FILL_AUTO_VALUE, type, project.c_str(), file.c_str(), line, "%s",
                text.c_str());

  // see comment in common.h
  NBDCOMPILE_ASSERT((uint32_t)LogType::Debug == (uint32_t)LogType__Internal::Debug,
                    "External and internal LogType enums must match");
  NBDCOMPILE_ASSERT((uint32_t)LogType::Comment == (uint32_t)LogType__Internal::Comment,
                    "External and internal LogType enums must match");
  NBDCOMPILE_ASSERT((uint32_t)LogType::Warning == (uint32_t)LogType__Internal::Warning,
                    "External and internal LogType enums must match");
  NBDCOMPILE_ASSERT((uint32_t)LogType::Error == (uint32_t)LogType__Internal::Error,
                    "External and internal LogType enums must match");
  NBDCOMPILE_ASSERT((uint32_t)LogType::Fatal == (uint32_t)LogType__Internal::Fatal,
                    "External and internal LogType enums must match");
  NBDCOMPILE_ASSERT((uint32_t)LogType::Count == (uint32_t)LogType__Internal::Count,
                    "External and internal LogType enums must match");
  NBDCOMPILE_ASSERT(arraydim<LogType>() == 5, "External and internal LogType enums must match");

#if ENABLED(DEBUGBREAK_ON_ERROR_LOG)
  if(type == LogType::Error)
    NBDBREAK();
#endif

  if(type == LogType::Fatal)
    NBDDUMP();
}

extern "C" NOOBDAWN_API const char *NOOBDAWN_CC NOOBDAWN_GetLogFile()
{
  return NBDGETLOGFILE();
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_GetLogFileContents(uint64_t offset,
                                                                        nbdstr &logfile)
{
  logfile = FileIO::logfile_readall(offset, NBDGETLOGFILE());
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_InitialiseReplay(GlobalEnvironment env,
                                                                      const nbdarray<nbdstr> &args)
{
  NoobDawn::Inst().InitialiseReplay(env, args);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_ShutdownReplay()
{
  {
    SCOPED_LOCK(detailStringLock);
    for(const nbdstr *msg : detailStrings)
      delete msg;
    detailStrings.clear();
  }

  NoobDawn::Inst().ShutdownReplay();
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_CreateBugReport(const nbdstr &logfile,
                                                                     const nbdstr &dumpfile,
                                                                     nbdstr &report)
{
  mz_zip_archive zip;
  NBDEraseEl(zip);

  if(report.empty())
  {
    report = FileIO::GetTempFolderFilename() +
             StringFormat::sntimef(Timing::GetUTCTime(), "/noobdawn_report_%H%M%S.zip");
  }

  FileIO::Delete(report);

  mz_zip_writer_init_file(&zip, report.c_str(), 0);

  if(!dumpfile.empty())
    mz_zip_writer_add_file(&zip, "minidump.dmp", dumpfile.c_str(), NULL, 0, MZ_BEST_COMPRESSION);

  if(!logfile.empty())
  {
    nbdstr contents = FileIO::logfile_readall(0, logfile);
    mz_zip_writer_add_mem(&zip, "error.log", contents.data(), contents.length(), MZ_BEST_COMPRESSION);
  }

  mz_zip_writer_finalize_archive(&zip);
  mz_zip_writer_end(&zip);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_RegisterMemoryRegion(void *base, size_t size)
{
  NoobDawn::Inst().RegisterMemoryRegion(base, size);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_UnregisterMemoryRegion(void *base)
{
  NoobDawn::Inst().UnregisterMemoryRegion(base);
}

extern "C" NOOBDAWN_API ExecuteResult NOOBDAWN_CC
NOOBDAWN_ExecuteAndInject(const nbdstr &app, const nbdstr &workingDir, const nbdstr &cmdLine,
                           const nbdarray<EnvironmentModification> &env, const nbdstr &capturefile,
                           const CaptureOptions &opts, const nbdstr &blacklist, bool waitForExit)
{
  nbdpair<RDResult, uint32_t> status = Process::LaunchAndInjectIntoProcess(
      app, workingDir, cmdLine, env, capturefile, opts, blacklist, waitForExit != 0);

  ExecuteResult ret;
  ret.result = status.first;
  ret.ident = status.second;
  return ret;
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_GetDefaultCaptureOptions(CaptureOptions *opts)
{
  *opts = CaptureOptions();
}

extern "C" NOOBDAWN_API ResultDetails NOOBDAWN_CC NOOBDAWN_StartGlobalHook(
    const nbdstr &pathmatch, const nbdstr &capturefile, const CaptureOptions &opts)
{
  return Process::StartGlobalHook(pathmatch, capturefile, opts);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_StopGlobalHook()
{
  Process::StopGlobalHook();
}

extern "C" NOOBDAWN_API bool NOOBDAWN_CC NOOBDAWN_IsGlobalHookActive()
{
  return Process::IsGlobalHookActive();
}

extern "C" NOOBDAWN_API bool NOOBDAWN_CC NOOBDAWN_CanGlobalHook()
{
  return Process::CanGlobalHook();
}

extern "C" NOOBDAWN_API ExecuteResult NOOBDAWN_CC
NOOBDAWN_InjectIntoProcess(uint32_t pid, const nbdarray<EnvironmentModification> &env,
                            const nbdstr &capturefile, const CaptureOptions &opts,
                            const nbdstr &blacklist, bool waitForExit)
{
  nbdpair<RDResult, uint32_t> status =
      Process::InjectIntoProcess(pid, env, capturefile, opts, blacklist, waitForExit != 0);

  ExecuteResult ret;
  ret.result = status.first;
  ret.ident = status.second;
  return ret;
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_FreeArrayMem(void *mem)
{
  free(mem);
}

// not exported, this is needed for calling from the container allocate functions
void NOOBDAWN_OutOfMemory(uint64_t sz)
{
  NBDFATAL("Allocation failed for %llu bytes", sz);
}

extern "C" NOOBDAWN_API void *NOOBDAWN_CC NOOBDAWN_AllocArrayMem(uint64_t sz)
{
  void *ret = malloc((size_t)sz);
  if(ret == NULL)
    NOOBDAWN_OutOfMemory(sz);
  return ret;
}

extern "C" NOOBDAWN_API uint32_t NOOBDAWN_CC NOOBDAWN_EnumerateRemoteTargets(const nbdstr &URL,
                                                                                uint32_t nextIdent)
{
  nbdstr host = "localhost";
  if(!URL.empty())
    host = URL;

  nbdstr deviceID = host;

  // initial case is we're called with 0, start with the first port.
  // otherwise we're called with the last successful ident, so increment
  // before continuing to enumerate.
  if(nextIdent == 0)
    nextIdent = NoobDawn_FirstTargetControlPort;
  else
    nextIdent++;

  IDeviceProtocolHandler *protocol = NoobDawn::Inst().GetDeviceProtocol(deviceID);

  if(protocol)
  {
    deviceID = protocol->GetDeviceID(deviceID);
    host = protocol->RemapHostname(deviceID);
    if(host.empty())
      return 0;
  }
  else
  {
    // hosts specified with a port are supported only for replay, do not enumerate targets on those
    // hosts
    if(URL.contains(':'))
      return 0;
  }

  for(; nextIdent <= NoobDawn_LastTargetControlPort; nextIdent++)
  {
    uint16_t port = (uint16_t)nextIdent;
    if(protocol)
      port = protocol->RemapPort(deviceID, port);

    if(port == 0)
      return 0;

    Network::Socket *sock = Network::CreateClientSocket(host, port, 250);

    if(sock)
    {
      if(protocol)
      {
        Threading::Sleep(100);
        (void)sock->IsRecvDataWaiting();
        if(!sock->Connected())
        {
          SAFE_DELETE(sock);
          return 0;
        }
      }

      SAFE_DELETE(sock);
      return nextIdent;
    }
  }

  // tried all idents remaining and found nothing
  return 0;
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC
NOOBDAWN_GetSupportedDeviceProtocols(nbdarray<nbdstr> *supportedProtocols)
{
  *supportedProtocols = NoobDawn::Inst().GetSupportedDeviceProtocols();
}

extern "C" NOOBDAWN_API IDeviceProtocolController *NOOBDAWN_CC
NOOBDAWN_GetDeviceProtocolController(const nbdstr &protocol)
{
  return NoobDawn::Inst().GetDeviceProtocol(protocol);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_BecomeRemoteServer(
    const nbdstr &listenhost, uint16_t port, NOOBDAWN_KillCallback killReplay,
    NOOBDAWN_PreviewWindowCallback previewWindow)
{
  // ensure a sensible default if no callback is provided, that just never kills
  if(!killReplay)
    killReplay = []() { return false; };

  // ditto for preview windows
  if(!previewWindow)
    previewWindow = [](bool, const nbdarray<WindowingSystem> &) {
      WindowingData ret = {WindowingSystem::Unknown};
      return ret;
    };

  if(port == 0)
    port = NoobDawn_RemoteServerPort;

  NoobDawn::Inst().BecomeRemoteServer(listenhost.empty() ? "0.0.0.0" : listenhost, port,
                                       killReplay, previewWindow);
}

extern "C" NOOBDAWN_API bool NOOBDAWN_CC NOOBDAWN_CanSelfHostedCapture(const nbdstr &dllname)
{
  return Process::IsModuleLoaded(dllname);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_StartSelfHostCapture(const nbdstr &dllname)
{
  if(!Process::IsModuleLoaded(dllname))
    return;

  void *module = Process::LoadModule(dllname);

  if(module == NULL)
    return;

  pNOOBDAWN_GetAPI get =
      (pNOOBDAWN_GetAPI)Process::GetFunctionAddress(module, "NOOBDAWN_GetAPI");

  if(get == NULL)
    return;

  NOOBDAWN_API_1_0_0 *rdoc = NULL;

  get(eNOOBDAWN_API_Version_1_0_0, (void **)&rdoc);

  if(rdoc == NULL)
    return;

  rdoc->StartFrameCapture(NULL, NULL);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_EndSelfHostCapture(const nbdstr &dllname)
{
  if(!Process::IsModuleLoaded(dllname))
    return;

  void *module = Process::LoadModule(dllname);

  if(module == NULL)
    return;

  pNOOBDAWN_GetAPI get =
      (pNOOBDAWN_GetAPI)Process::GetFunctionAddress(module, "NOOBDAWN_GetAPI");

  if(get == NULL)
    return;

  NOOBDAWN_API_1_0_0 *rdoc = NULL;

  get(eNOOBDAWN_API_Version_1_0_0, (void **)&rdoc);

  if(rdoc == NULL)
    return;

  rdoc->EndFrameCapture(NULL, NULL);
}

extern "C" NOOBDAWN_API bool NOOBDAWN_CC
NOOBDAWN_NeedVulkanLayerRegistration(VulkanLayerRegistrationInfo *info)
{
  VulkanLayerFlags flags = VulkanLayerFlags::NoFlags;
  nbdarray<nbdstr> myJSONs;
  nbdarray<nbdstr> otherJSONs;

  bool ret = NoobDawn::Inst().NeedVulkanLayerRegistration(flags, myJSONs, otherJSONs);

  if(info)
  {
    info->flags = flags;

    info->myJSONs.resize(myJSONs.size());
    for(size_t i = 0; i < myJSONs.size(); i++)
      info->myJSONs[i] = myJSONs[i];

    info->otherJSONs.resize(otherJSONs.size());
    for(size_t i = 0; i < otherJSONs.size(); i++)
      info->otherJSONs[i] = otherJSONs[i];
  }

  return ret;
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_UpdateVulkanLayerRegistration(bool systemLevel)
{
  NoobDawn::Inst().UpdateVulkanLayerRegistration(systemLevel);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_UpdateInstalledVersionNumber()
{
#if ENABLED(RDOC_WIN32)
  HKEY key = NULL;

  LSTATUS ret =
      RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                      0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL);

  if(ret != ERROR_SUCCESS)
  {
    if(key)
      RegCloseKey(key);

    return;
  }

  bool done = false;

  char guidName[256] = {};
  for(DWORD idx = 0; !done; idx++)
  {
    // enumerate all the uninstall keys
    ret = RegEnumKeyA(key, idx, guidName, sizeof(guidName) - 1);

    if(ret == ERROR_NO_MORE_ITEMS)
    {
      break;
    }
    else if(ret != ERROR_SUCCESS)
    {
      break;
    }

    // open the key as we'll need it for RegSetValueExA
    HKEY subkey = NULL;
    ret = RegCreateKeyExA(key, guidName, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &subkey, NULL);

    if(ret == ERROR_SUCCESS && subkey)
    {
      char DisplayName[256] = {};
      char Publisher[256] = {};
      DWORD len = sizeof(DisplayName) - 1;

      // fetch DisplayName and Publisher values
      ret = RegGetValueA(subkey, NULL, "DisplayName", RRF_RT_ANY, NULL, DisplayName, &len);

      // allow the value to silently not exist
      if(ret != ERROR_SUCCESS)
        DisplayName[0] = 0;

      len = sizeof(Publisher) - 1;
      ret = RegGetValueA(subkey, NULL, "Publisher", RRF_RT_ANY, NULL, Publisher, &len);

      if(ret != ERROR_SUCCESS)
        Publisher[0] = 0;

      // if this is our key, set the version number
      if(!strcmp(DisplayName, "NoobDawn") && !strcmp(Publisher, "Baldur Karlsson"))
      {
        DWORD Version = (NOOBDAWN_VERSION_MAJOR << 24) | (NOOBDAWN_VERSION_MINOR << 16);
        DWORD VersionMajor = NOOBDAWN_VERSION_MAJOR;
        DWORD VersionMinor = NOOBDAWN_VERSION_MINOR;
        nbdstr DisplayVersion = MAJOR_MINOR_VERSION_STRING ".0";

        RegSetValueExA(subkey, "Version", 0, REG_DWORD, (const BYTE *)&Version, sizeof(Version));
        RegSetValueExA(subkey, "VersionMajor", 0, REG_DWORD, (const BYTE *)&VersionMajor,
                       sizeof(VersionMajor));
        RegSetValueExA(subkey, "VersionMinor", 0, REG_DWORD, (const BYTE *)&VersionMinor,
                       sizeof(VersionMinor));
        RegSetValueExA(subkey, "DisplayVersion", 0, REG_SZ, (const BYTE *)DisplayVersion.c_str(),
                       (DWORD)DisplayVersion.size() + 1);
        done = true;
      }
    }

    if(subkey)
      RegCloseKey(subkey);
  }

  if(key)
    RegCloseKey(key);

#endif
}

static nbdstr ResourceFormatName(const ResourceFormat &fmt)
{
  nbdstr ret;

  if(fmt.Special())
  {
    switch(fmt.type)
    {
      case ResourceFormatType::Regular: break;
      case ResourceFormatType::Undefined: return "Undefined";
      case ResourceFormatType::BC1:
        if(fmt.compType == CompType::Typeless)
          return "BC1_TYPELESS";
        return fmt.SRGBCorrected() ? "BC1_SRGB" : "BC1_UNORM";
      case ResourceFormatType::BC2:
        if(fmt.compType == CompType::Typeless)
          return "BC2_TYPELESS";
        return fmt.SRGBCorrected() ? "BC2_SRGB" : "BC2_UNORM";
      case ResourceFormatType::BC3:
        if(fmt.compType == CompType::Typeless)
          return "BC3_TYPELESS";
        return fmt.SRGBCorrected() ? "BC3_SRGB" : "BC3_UNORM";
      case ResourceFormatType::BC4:
        if(fmt.compType == CompType::Typeless)
          return "BC4_TYPELESS";
        return fmt.compType == CompType::UNorm ? "BC4_UNORM" : "BC4_SNORM";
      case ResourceFormatType::BC5:
        if(fmt.compType == CompType::Typeless)
          return "BC5_TYPELESS";
        return fmt.compType == CompType::UNorm ? "BC5_UNORM" : "BC5_SNORM";
      case ResourceFormatType::BC6:
        if(fmt.compType == CompType::Typeless)
          return "BC6_TYPELESS";
        return fmt.compType == CompType::UNorm ? "BC6_UFLOAT" : "BC6_SFLOAT";
      case ResourceFormatType::BC7:
        if(fmt.compType == CompType::Typeless)
          return "BC7_TYPELESS";
        return fmt.SRGBCorrected() ? "BC7_SRGB" : "BC7_UNORM";
      case ResourceFormatType::ETC2:
      {
        if(fmt.compCount == 4)
          return fmt.SRGBCorrected() ? "ETC2_RGB8A1_SRGB" : "ETC2_RGB8A1_UNORM";
        else
          return fmt.SRGBCorrected() ? "ETC2_RGB8_SRGB" : "ETC2_RGB8_UNORM";
      }
      case ResourceFormatType::EAC:
      {
        if(fmt.compCount == 1)
          return fmt.compType == CompType::UNorm ? "EAC_R11_UNORM" : "EAC_R11_SNORM";
        else if(fmt.compCount == 2)
          return fmt.compType == CompType::UNorm ? "EAC_RG11_UNORM" : "EAC_RG11_SNORM";
        else
          return fmt.SRGBCorrected() ? "ETC2_EAC_RGBA8_SRGB" : "ETC2_EAC_RGBA8_UNORM";
      }
      case ResourceFormatType::ASTC: return fmt.SRGBCorrected() ? "ASTC_SRGB" : "ASTC_UNORM";
      // 10:10:10 A2 is the only format that can have all the usual format types (unorm, snorm,
      // etc). So we break and handle it like any other format below.
      case ResourceFormatType::R10G10B10A2:
        ret = fmt.BGRAOrder() ? "B10G10R10A2" : "R10G10B10A2";
        break;
      case ResourceFormatType::R11G11B10: return "R11G11B10_FLOAT";
      case ResourceFormatType::R5G6B5: return fmt.BGRAOrder() ? "B5G6R5_UNORM" : "R5G6B5_UNORM";
      case ResourceFormatType::R5G5B5A1:
        return fmt.BGRAOrder() ? "B5G5R5A1_UNORM" : "R5G5B5A1_UNORM";
      case ResourceFormatType::R9G9B9E5: return "R9G9B9E5_FLOAT";
      case ResourceFormatType::R4G4B4A4:
        return fmt.BGRAOrder() ? "B4G4R4A4_UNORM" : "R4G4B4A4_UNORM";
      case ResourceFormatType::R4G4: return "R4G4_UNORM";
      case ResourceFormatType::D16S8:
        return fmt.compType == CompType::Typeless ? "D16S8_TYPELESS" : "D16S8";
      case ResourceFormatType::D24S8:
        return fmt.compType == CompType::Typeless ? "D24S8_TYPELESS" : "D24S8";
      case ResourceFormatType::D32S8:
        return fmt.compType == CompType::Typeless ? "D32S8_TYPELESS" : "D32S8";
      case ResourceFormatType::S8: return "S8";
      case ResourceFormatType::A8: return "A8_UNORM";
      case ResourceFormatType::YUV8:
      case ResourceFormatType::YUV10:
      case ResourceFormatType::YUV12:
      case ResourceFormatType::YUV16:
      {
        int yuvbits = 0;

        switch(fmt.type)
        {
          case ResourceFormatType::YUV8: yuvbits = 8; break;
          case ResourceFormatType::YUV10: yuvbits = 10; break;
          case ResourceFormatType::YUV12: yuvbits = 12; break;
          case ResourceFormatType::YUV16: yuvbits = 16; break;
          default: break;
        }

        uint32_t planeCount = fmt.YUVPlaneCount();
        uint32_t subsampling = fmt.YUVSubsampling();

        // special case formats that don't match the FOURCC format
        if(yuvbits == 8 && planeCount == 2 && subsampling == 420)
          return "NV12";
        if(yuvbits == 8 && planeCount == 1 && subsampling == 444)
          return "AYUV";
        if(yuvbits == 8 && planeCount == 1 && subsampling == 422)
          return "YUY2";

        switch(subsampling)
        {
          case 444:
            if(planeCount == 1)
              return StringFormat::Fmt("Y4%02u", yuvbits);
            else if(planeCount == 2)
              return StringFormat::Fmt("P4%02u", yuvbits);
            else
              return StringFormat::Fmt("YUV444_%uPlane_%ubit", planeCount, yuvbits);
          case 422:
            if(planeCount == 1)
              return StringFormat::Fmt("Y2%02u", yuvbits);
            else if(planeCount == 2)
              return StringFormat::Fmt("P2%02u", yuvbits);
            else
              return StringFormat::Fmt("YUV422_%uPlane_%ubit", planeCount, yuvbits);
          case 420:
            if(planeCount == 1)
              return StringFormat::Fmt("Y0%02u", yuvbits);
            else if(planeCount == 2)
              return StringFormat::Fmt("P0%02u", yuvbits);
            else
              return StringFormat::Fmt("YUV420_%uPlane_%ubit", planeCount, yuvbits);
          default: NBDERR("Unexpected YUV Subsampling amount %u", subsampling);
        }

        return StringFormat::Fmt("YUV_%u_%uPlane_%ubit", subsampling, planeCount, yuvbits);
      }
      case ResourceFormatType::PVRTC: return "PVRTC";
    }
  }
  else if(fmt.compType == CompType::Depth)
  {
    ret = StringFormat::Fmt("D%u", fmt.compByteWidth * 8);
  }
  else
  {
    char comps[] = "RGBA";

    if(fmt.BGRAOrder())
      std::swap(comps[0], comps[2]);

    for(uint32_t i = 0; i < fmt.compCount; i++)
      ret += StringFormat::Fmt("%c%u", comps[i], fmt.compByteWidth * 8);
  }

  switch(fmt.compType)
  {
    case CompType::Typeless: return ret + "_TYPELESS";
    case CompType::Float: return ret + "_FLOAT";
    case CompType::UNorm: return ret + "_UNORM";
    case CompType::SNorm: return ret + "_SNORM";
    case CompType::UInt: return ret + "_UINT";
    case CompType::SInt: return ret + "_SINT";
    case CompType::UScaled: return ret + "_USCALED";
    case CompType::SScaled: return ret + "_SSCALED";
    case CompType::UNormSRGB: return ret + "_SRGB";
    case CompType::Depth:
      // we already special-cased depth component type above to be Dx instead of Rx
      return ret;
  }

  // should never get here
  NBDERR("Unhandled format component type");
  return ret + "_UNKNOWN";
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_ResourceFormatName(const ResourceFormat &fmt,
                                                                        nbdstr &name)
{
  name = ResourceFormatName(fmt);
}

static void TestPrintMsg(const nbdstr &msg)
{
  OSUtility::WriteOutput(OSUtility::Output_DebugMon, msg.c_str());
  OSUtility::WriteOutput(OSUtility::Output_StdErr, msg.c_str());
}

extern "C" NOOBDAWN_API int NOOBDAWN_CC NOOBDAWN_RunFunctionalTests(const nbdarray<nbdstr> &args)
{
#if ENABLED(RDOC_WIN32)
  const char *moduledir = "/pymodules";
  const char *modulename = "noobdawn.pyd";
  nbdstr pythonlibs[] = {"python3?.dll"};
#elif ENABLED(RDOC_LINUX)
  const char *moduledir = "";
  const char *modulename = "noobdawn.so";
  // we don't care about pymalloc or not
  nbdstr pythonlibs[] = {"libpython3.?m.so.1.0", "libpython3.?.so.1.0", "libpython3.?m.so",
                         "libpython3.?.so"};
#elif ENABLED(RDOC_APPLE)
  const char *moduledir = "";
  const char *modulename = "noobdawn.so";
  nbdstr pythonlibs[] = {"libpython3.?.dylib"};
#else
  const char *moduledir = "";
  const char *modulename = "";
  nbdstr pythonlibs[] = {};
  TestPrintMsg(
      "Running functional tests not directly supported on this platform.\n"
      "Try running util/test/run_tests.py manually.\n");
  return 1;
#endif

  nbdstr libPath;
  FileIO::GetLibraryFilename(libPath);

  libPath = get_dirname(libPath);
  nbdstr modulePath = libPath + moduledir;

  nbdstr moduleFilename = modulePath + "/" + modulename;

  if(!FileIO::exists(moduleFilename))
  {
    TestPrintMsg(StringFormat::Fmt("Couldn't locate python module at %s\n", moduleFilename.c_str()));
    return 1;
  }

  void *moduleHandle = Process::LoadModule(moduleFilename);

  int pythonMinorVersion = 0;

  if(moduleHandle)
  {
    typedef int (*PFN_rd_python_minor_version)();

    PFN_rd_python_minor_version py_ver_minor =
        (PFN_rd_python_minor_version)Process::GetFunctionAddress(moduleHandle,
                                                                 "_rd_python_minor_version");

    pythonMinorVersion = py_ver_minor();
  }
  else
  {
    TestPrintMsg(StringFormat::Fmt("Couldn't load python module at %s\n", moduleFilename.c_str()));
    return 1;
  }

  // if we've been built either on windows or on linux from within the project root, going up two
  // directories from the library will put us at the project root. This is the most common scenario
  // and we don't add handling for locating the script elsewhere as in that case the user can run it
  // directly. This is just intended as a useful shortcut for common cases.
  nbdstr scriptPath = libPath + "/../../util/test/run_tests.py";

  if(!FileIO::exists(scriptPath))
  {
    TestPrintMsg(StringFormat::Fmt("Couldn't locate run_tests.py script at %s\n", scriptPath.c_str()));
    return 1;
  }

  void *handle = NULL;

  for(nbdstr py : pythonlibs)
  {
    // patch up the python minor version
    const int32_t idx = py.find('?');
    if(idx == -1)
    {
      TestPrintMsg(StringFormat::Fmt("Python library pattern missing placeholder: %s\n", py.c_str()));
      return 1;
    }
    py.replace(idx, 1, StringFormat::Fmt("%d", pythonMinorVersion));

    handle = Process::LoadModule(py);
    if(handle)
    {
      NBDLOG("Loaded python from %s", py.c_str());
      break;
    }
  }

  if(!handle)
  {
    TestPrintMsg("Couldn't locate python 3.6 library\n");
    return 1;
  }

  typedef int(NOOBDAWN_CC * PFN_Py_Main)(int, wchar_t **);

  PFN_Py_Main mainFunc = (PFN_Py_Main)Process::GetFunctionAddress(handle, "Py_Main");

  if(!mainFunc)
  {
    TestPrintMsg("Couldn't get Py_Main in python library\n");
    return 1;
  }

  nbdarray<nbdwstr> wideArgs;
  wideArgs.resize(args.size());

  for(size_t i = 0; i < args.size(); i++)
    wideArgs[i] = StringFormat::UTF82Wide(args[i]);

  // insert fake arguments to point at the script and our modules
  wideArgs.insert(0, {
                         L"python",
                         // specify script path
                         StringFormat::UTF82Wide(scriptPath),
                         // specify native library path
                         L"--noobdawn",
                         StringFormat::UTF82Wide(libPath),
                         // specify python module path
                         L"--pynoobdawn",
                         StringFormat::UTF82Wide(modulePath),
                         // force in-process as we can't fork out to python to pass args
                         L"--in-process",
                     });

  nbdarray<wchar_t *> wideArgStrings;
  wideArgStrings.resize(wideArgs.size());

  for(size_t i = 0; i < wideArgs.size(); i++)
    wideArgStrings[i] = wideArgs[i].data();

  return mainFunc((int)wideArgStrings.size(), wideArgStrings.data());
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_BeginProfileRegion(const nbdstr &name)
{
  Superluminal::BeginProfileRange(name);
}

extern "C" NOOBDAWN_API void NOOBDAWN_CC NOOBDAWN_EndProfileRegion()
{
  Superluminal::EndProfileRange();
}
