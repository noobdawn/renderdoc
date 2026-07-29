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

#include "core/core.h"
#include <time.h>
#include <algorithm>
#include "api/replay/version.h"
#include "common/common.h"
#include "common/threading.h"
#include "core/settings.h"
#include "hooks/hooks.h"
#include "jpeg-compressor/jpge.h"
#include "maths/formatpacking.h"
#include "replay/replay_driver.h"
#include "serialise/nbdfile.h"
#include "serialise/serialiser.h"
#include "stb/stb_image_write.h"
#include "strings/string_utils.h"
#include "superluminal/superluminal.h"
#include "crash_handler.h"

#include "api/replay/noobdawn_tostr.inl"

#include "api/replay/pipestate.inl"

#include "replay/noobdawn_serialise.inl"

extern "C" const nbdstr VulkanLayerJSONBasename = STRINGIZE(RDOC_BASE_NAME);

RDOC_DEBUG_CONFIG(bool, Capture_Debug_SnapshotDiagnosticLog, false,
                  "Snapshot the diagnostic log at capture time and embed in the capture.");

RDOC_CONFIG(bool, Capture_IncludeExtendedThumbnail, false,
            "Save the thumbnail unresized and losslessly encoded during capture.");

RDOC_CONFIG(bool, Replay_Debug_PrintChunkTimings, false, "Print stats of chunk processing times");

RDOC_CONFIG(bool, Replay_Debug_SingleThreadedCompilation, false,
            "Compile all shaders and PSOs single-threaded.");

// this is declared centrally so it can be shared with any backend - the name is a misnomer but kept
// for backwards compatibility reasons.
RDOC_CONFIG(nbdarray<nbdstr>, DXBC_Debug_SearchDirPaths, {},
            "Paths to search for separated shader debug PDBs, including all types of paths.");
RDOC_CONFIG(nbdarray<nbdstr>, Replay_Shader_LimitedSearchDirPaths, {},
            "Companion array to DXBC.Debug.SearchDirPaths - listing paths which should not be "
            "searched exhaustively but only used for simple lookups.");

void WriteAnnotation(SDObject *obj, NOOBDAWN_AnnotationType valueType, uint32_t valueVectorWidth,
                     NOOBDAWN_AnnotationValue value)
{
  if(valueType == eNOOBDAWN_Empty)
  {
    NBDERR("Invalid type of annotation to write");
    return;
  }

  const nbdinflexiblestr types[eNOOBDAWN_APIObject + 1][4] = {
      // eNOOBDAWN_Empty,
      {""_lit, ""_lit, ""_lit, ""_lit},
      // eNOOBDAWN_Bool,
      {"bool"_lit, "bool2"_lit, "bool3"_lit, "bool4"_lit},
      // eNOOBDAWN_Int32,
      {"int"_lit, "int2"_lit, "int3"_lit, "int4"_lit},
      // eNOOBDAWN_UInt32,
      {"uint"_lit, "uint2"_lit, "uint3"_lit, "uint4"_lit},
      // eNOOBDAWN_Int64,
      {"long"_lit, "long2"_lit, "long3"_lit, "long4"_lit},
      // eNOOBDAWN_UInt64,
      {"ulong"_lit, "ulong2"_lit, "ulong3"_lit, "ulong4"_lit},
      // eNOOBDAWN_Float,
      {"float"_lit, "float2"_lit, "float3"_lit, "float4"_lit},
      // eNOOBDAWN_Double,
      {"double"_lit, "double2"_lit, "double3"_lit, "double4"_lit},
      // eNOOBDAWN_String,
      {"string"_lit, ""_lit, ""_lit, ""_lit},
      // eNOOBDAWN_APIObject,
      {"ResourceId"_lit, ""_lit, ""_lit, ""_lit},
  };

  const uint32_t byteSize[eNOOBDAWN_APIObject + 1] = {
      // eNOOBDAWN_Empty,
      0,
      // eNOOBDAWN_Bool,
      1,
      // eNOOBDAWN_Int32,
      4,
      // eNOOBDAWN_UInt32,
      4,
      // eNOOBDAWN_Int64,
      8,
      // eNOOBDAWN_UInt64,
      8,
      // eNOOBDAWN_Float,
      4,
      // eNOOBDAWN_Double,
      8,
      // eNOOBDAWN_String,
      0,
      // eNOOBDAWN_APIObject,
      8,
  };

  const SDBasic basetype[eNOOBDAWN_APIObject + 1] = {
      // eNOOBDAWN_Empty,
      SDBasic::Null,
      // eNOOBDAWN_Bool,
      SDBasic::Boolean,
      // eNOOBDAWN_Int32,
      SDBasic::SignedInteger,
      // eNOOBDAWN_UInt32,
      SDBasic::UnsignedInteger,
      // eNOOBDAWN_Int64,
      SDBasic::SignedInteger,
      // eNOOBDAWN_UInt64,
      SDBasic::UnsignedInteger,
      // eNOOBDAWN_Float,
      SDBasic::Float,
      // eNOOBDAWN_Double,
      SDBasic::Float,
      // eNOOBDAWN_String,
      SDBasic::String,
      // eNOOBDAWN_APIObject,
      SDBasic::Resource,
  };

  if(valueVectorWidth > 1)
  {
    if(valueType == eNOOBDAWN_APIObject)
    {
      NBDERR("Invalid vector width for API object");
      return;
    }
    else if(valueType == eNOOBDAWN_String)
    {
      NBDERR("Invalid vector width for string");
      return;
    }

    const nbdinflexiblestr comps[] = {"1"_lit, "2"_lit, "3"_lit, "4"_lit};

    obj->type.basetype = SDBasic::Struct;
    obj->type.name = types[valueType][valueVectorWidth - 1];

    NOOBDAWN_AnnotationValue tmp = {};
    for(uint32_t i = 0; i < valueVectorWidth; i++)
    {
      SDObject *child = obj->CreateChildByKeyPath(comps[i]);

      if(byteSize[valueType] == 1)
        memcpy(&tmp.boolean, &value.vector.boolean[i], sizeof(tmp.boolean));
      if(byteSize[valueType] == 4)
        memcpy(&tmp.uint32, &value.vector.uint32[i], sizeof(tmp.uint32));
      if(byteSize[valueType] == 8)
        memcpy(&tmp.uint64, &value.vector.uint64[i], sizeof(tmp.uint64));

      WriteAnnotation(child, valueType, 0, tmp);
      if(i == 0)
        obj->type.byteSize = child->type.byteSize * valueVectorWidth;
    }

    return;
  }

  obj->type.name = types[valueType][0];
  obj->type.basetype = basetype[valueType];
  obj->type.byteSize = byteSize[valueType];

  switch(valueType)
  {
    case eNOOBDAWN_Empty:
    case eNOOBDAWN_AnnotationMax: NBDERR("Invalid annotation type"); return;
    case eNOOBDAWN_Bool:
    {
      obj->data.basic.b = value.boolean;
      break;
    }
    case eNOOBDAWN_Int32:
    {
      obj->data.basic.i = value.int32;
      break;
    }
    case eNOOBDAWN_UInt32:
    {
      obj->data.basic.u = value.uint32;
      break;
    }
    case eNOOBDAWN_Int64:
    {
      obj->data.basic.u = value.int64;
      break;
    }
    case eNOOBDAWN_UInt64:
    {
      obj->data.basic.u = value.uint64;
      break;
    }
    case eNOOBDAWN_Float:
    {
      obj->data.basic.d = value.float32;
      break;
    }
    case eNOOBDAWN_Double:
    {
      obj->data.basic.d = value.float64;
      break;
    }
    case eNOOBDAWN_String:
    {
      obj->type.byteSize = strlen(value.string);
      obj->data.str = value.string;
      break;
    }
    case eNOOBDAWN_APIObject:
    {
      memcpy(&obj->data.basic.id, &value.uint64, sizeof(ResourceId));
      break;
    }
  }
}

void LogReplayOptions(const ReplayOptions &opts)
{
  NBDLOG("%s API validation during replay", (opts.apiValidation ? "Enabling" : "Not enabling"));

  if(opts.forceGPUVendor == GPUVendor::Unknown && opts.forceGPUDeviceID == 0 &&
     opts.forceGPUDriverName.empty())
  {
    NBDLOG("Using default GPU replay selection algorithm");
  }
  else
  {
    NBDLOG("Overriding GPU replay selection:");
    NBDLOG("  Vendor %s, device %u, driver \"%s\"", ToStr(opts.forceGPUVendor).c_str(),
           opts.forceGPUDeviceID, opts.forceGPUDriverName.c_str());
  }

  NBDLOG("Replay optimisation level: %s", ToStr(opts.optimisation).c_str());
}

// these one is done by hand as we format it
template <>
nbdstr DoStringise(const ResourceId &el)
{
  NBDCOMPILE_ASSERT(sizeof(el) == sizeof(uint64_t), "ResourceId is no longer 1:1 with uint64_t");

  // below is equivalent to:
  // return StringFormat::Fmt("ResourceId::%llu", el);

  uint64_t num = 0;
  memcpy(&num, &el, sizeof(uint64_t));

#define PREFIX "ResourceId::"

  // hardcode empty/null ResourceId to both avoid special case below and fast-path a common case as
  // a string literal.
  if(num == 0)
    return PREFIX "0"_lit;

  // enough for prefix and a 64-bit value in decimal
  char str[48] = {};

  NBDCOMPILE_ASSERT(ARRAY_COUNT(str) > sizeof(PREFIX) + 20, "Scratch buffer is not large enough");

  // ARRAY_COUNT(str) - 1 would point us at the last element, we go one further back to leave a
  // trailing NUL character
  char *c = str + ARRAY_COUNT(str) - 2;

  // build up digits in reverse order from the end of the buffer
  while(num)
  {
    *(c--) = char((num % 10) + '0');
    num /= 10;
  }

  // the length is sizeof(PREFIX) - 1, the index of the last actual character is - 2. Saves us a -1
  // in the loop below.
  const size_t prefixlast = sizeof(PREFIX) - 2;

  // add the prefix (in reverse order)
  for(size_t i = 0; i <= prefixlast; i++)
    *(c--) = PREFIX[prefixlast - i];

#undef PREFIX

  // the loop will have stepped us to the first NULL before our string, so return c+1
  return c + 1;
}

template <>
nbdstr DoStringise(const PointerVal &el)
{
  if(el.shader != ResourceId())
  {
    // we don't want to format as an ID, we need to encode the raw value
    uint64_t num;
    memcpy(&num, &el.shader, sizeof(num));

    return StringFormat::Fmt("GPUAddress::%llu::%llu::%u", el.pointer, num, el.pointerTypeID);
  }
  else
  {
    return StringFormat::Fmt("GPUAddress::%llu", el.pointer);
  }
}

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, ResourceId &el)
{
  ser.SerialiseValue(SDBasic::Resource, 8, (uint64_t &)el);
}

INSTANTIATE_SERIALISE_TYPE(ResourceId);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, RDResult &el)
{
  SERIALISE_MEMBER(code);
  SERIALISE_MEMBER(message);

  SIZE_CHECK(16);
}

INSTANTIATE_SERIALISE_TYPE(RDResult);

#if ENABLED(RDOC_LINUX) && ENABLED(RDOC_XLIB)
#include <X11/Xlib.h>
#endif

// from image_viewer.cpp
RDResult IMG_CreateReplayDevice(NBDFile *nbd, IReplayDriver **driver);

template <>
nbdstr DoStringise(const CaptureState &el)
{
  BEGIN_ENUM_STRINGISE(CaptureState);
  {
    STRINGISE_ENUM_CLASS(LoadingReplaying);
    STRINGISE_ENUM_CLASS(ActiveReplaying);
    STRINGISE_ENUM_CLASS(StructuredExport);
    STRINGISE_ENUM_CLASS(BackgroundCapturing);
    STRINGISE_ENUM_CLASS(ActiveCapturing);
  }
  END_ENUM_STRINGISE();
}

template <>
nbdstr DoStringise(const NBDDriver &el)
{
  BEGIN_ENUM_STRINGISE(NBDDriver);
  {
    STRINGISE_ENUM_CLASS(Unknown);
    STRINGISE_ENUM_CLASS(OpenGL);
    STRINGISE_ENUM_CLASS(OpenGLES);
    STRINGISE_ENUM_CLASS(Mantle);
    STRINGISE_ENUM_CLASS(D3D12);
    STRINGISE_ENUM_CLASS(D3D11);
    STRINGISE_ENUM_CLASS(D3D10);
    STRINGISE_ENUM_CLASS(D3D9);
    STRINGISE_ENUM_CLASS(D3D8);
    STRINGISE_ENUM_CLASS(Image);
    STRINGISE_ENUM_CLASS(Vulkan);
    STRINGISE_ENUM_CLASS(Metal);
  }
  END_ENUM_STRINGISE();
}

template <>
nbdstr DoStringise(const ReplayLogType &el)
{
  BEGIN_ENUM_STRINGISE(ReplayLogType);
  {
    STRINGISE_ENUM_NAMED(eReplay_Full, "Full replay including action");
    STRINGISE_ENUM_NAMED(eReplay_WithoutDraw, "Replay without action");
    STRINGISE_ENUM_NAMED(eReplay_OnlyDraw, "Replay only action");
  }
  END_ENUM_STRINGISE();
}

template <>
nbdstr DoStringise(const VendorExtensions &el)
{
  BEGIN_ENUM_STRINGISE(VendorExtensions);
  {
    STRINGISE_ENUM_CLASS(NvAPI);
    STRINGISE_ENUM_CLASS_NAMED(OpenGL_Ext, "Unsupported GL extensions");
    STRINGISE_ENUM_CLASS_NAMED(Vulkan_Ext, "Unsupported Vulkan extensions");
  }
  END_ENUM_STRINGISE();
}

template <>
nbdstr DoStringise(const NOOBDAWN_InputButton &el)
{
  char alphanumericbuf[2] = {'A', 0};

  // enums map straight to ascii
  if((el >= eNOOBDAWN_Key_A && el <= eNOOBDAWN_Key_Z) ||
     (el >= eNOOBDAWN_Key_0 && el <= eNOOBDAWN_Key_9))
  {
    alphanumericbuf[0] = (char)el;
    return alphanumericbuf;
  }

  BEGIN_ENUM_STRINGISE(NOOBDAWN_InputButton);
  {
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Divide, "/");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Multiply, "*");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Subtract, "-");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Plus, "+");

    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F1, "F1");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F2, "F2");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F3, "F3");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F4, "F4");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F5, "F5");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F6, "F6");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F7, "F7");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F8, "F8");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F9, "F9");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F10, "F10");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F11, "F11");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_F12, "F12");

    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Home, "Home");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_End, "End");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Insert, "Insert");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Delete, "Delete");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_PageUp, "PageUp");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_PageDn, "PageDn");

    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Backspace, "Backspace");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Tab, "Tab");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_PrtScrn, "PrtScrn");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Key_Pause, "Pause");
  }
  END_ENUM_STRINGISE();
}

template <>
nbdstr DoStringise(const SystemChunk &el)
{
  BEGIN_ENUM_STRINGISE(SystemChunk);
  {
    STRINGISE_ENUM_CLASS_NAMED(DriverInit, "Internal::Driver Initialisation Parameters");
    STRINGISE_ENUM_CLASS_NAMED(InitialContentsList, "Internal::List of Initial Contents Resources");
    STRINGISE_ENUM_CLASS_NAMED(InitialContents, "Internal::Initial Contents");
    STRINGISE_ENUM_CLASS_NAMED(CaptureBegin, "Internal::Beginning of Capture");
    STRINGISE_ENUM_CLASS_NAMED(CaptureScope, "Internal::Frame Metadata");
    STRINGISE_ENUM_CLASS_NAMED(CaptureEnd, "Internal::End of Capture");
  }
  END_ENUM_STRINGISE();
}

template <>
nbdstr DoStringise(const NOOBDAWN_AnnotationType &el)
{
  BEGIN_ENUM_STRINGISE(NOOBDAWN_AnnotationType);
  {
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Bool, "bool");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Int32, "int32");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_UInt32, "uint32");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Int64, "int64");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_UInt64, "uint64");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Float, "float");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_Double, "double");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_String, "string");
    STRINGISE_ENUM_NAMED(eNOOBDAWN_APIObject, "object");
  }
  END_ENUM_STRINGISE();
}

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, NOOBDAWN_AnnotationValue &el)
{
  if(ser.GetStructArg() == eNOOBDAWN_String)
  {
    SERIALISE_MEMBER(string).Hidden();
  }
  else
  {
    SERIALISE_MEMBER(vector.uint64).Hidden();
  }
}

INSTANTIATE_SERIALISE_TYPE(NOOBDAWN_AnnotationValue);

NoobDawn &NoobDawn::Inst()
{
  static NoobDawn realInst;
  return realInst;
}

void NoobDawn::RecreateCrashHandler()
{
  SCOPED_WRITELOCK(m_ExHandlerLock);

#if ENABLED(RDOC_CRASH_HANDLER)

  nbdstr exename;
  FileIO::GetExecutableFilename(exename);
  exename = strlower(exename);

  // only create crash handler when we're not in noobdawncmd (to prevent infinite loop as
  // the crash handler itself launches noobdawncmd)
  if(exename.contains("noobdawncmd"))
    return;

#if ENABLED(RDOC_WIN32)
  // there are way too many invalid reports coming from chrome, completely disable the crash handler
  // in that case.
  if(exename.find("chrome.exe") &&
     (GetModuleHandleA("chrome_elf.dll") || GetModuleHandleA("chrome_child.dll")))
  {
    NBDWARN("Disabling crash handling server due to detected chrome.");
    return;
  }

  // some people use vivaldi which is just chrome
  if(exename.find("vivaldi.exe") &&
     (GetModuleHandleA("vivaldi_elf.dll") || GetModuleHandleA("vivaldi_child.dll")))
  {
    NBDWARN("Disabling crash handling server due to detected chrome.");
    return;
  }

  // ditto opera
  if(exename.find("opera.exe") && GetModuleHandleA("opera_browser.dll"))
  {
    NBDWARN("Disabling crash handling server due to detected chrome.");
    return;
  }

  // ditto edge
  if(exename.find("msedge.exe") && GetModuleHandleA("msedge.dll"))
  {
    NBDWARN("Disabling crash handling server due to detected chrome.");
    return;
  }
#endif

  m_ExHandler = new CrashHandler(m_ExHandler);

  m_ExHandler->RegisterMemoryRegion(this, sizeof(NoobDawn));
#endif
}

void NoobDawn::UnloadCrashHandler()
{
  SCOPED_WRITELOCK(m_ExHandlerLock);

  if(!m_ExHandler)
    return;

  m_ExHandler->UnregisterMemoryRegion(this);

  SAFE_DELETE(m_ExHandler);
}

void NoobDawn::RegisterMemoryRegion(void *mem, size_t size)
{
  SCOPED_READLOCK(m_ExHandlerLock);

  if(m_ExHandler)
    m_ExHandler->RegisterMemoryRegion(mem, size);
}

void NoobDawn::UnregisterMemoryRegion(void *mem)
{
  SCOPED_READLOCK(m_ExHandlerLock);

  if(m_ExHandler)
    m_ExHandler->UnregisterMemoryRegion(mem);
}

NoobDawn::NoobDawn()
{
  m_CaptureFileTemplate = "";
  m_MarkerIndentLevel = 0;

  m_CapturesActive = 0;

  m_RemoteIdent = 0;
  m_RemoteThread = 0;

  m_Replay = false;

  m_Cap = 0;

  m_FocusKeys.clear();
  m_FocusKeys.push_back(eNOOBDAWN_Key_F11);

  m_CaptureKeys.clear();
  m_CaptureKeys.push_back(eNOOBDAWN_Key_F12);
  m_CaptureKeys.push_back(eNOOBDAWN_Key_PrtScrn);

  m_ExHandler = NULL;

  m_Overlay = eNOOBDAWN_Overlay_Default;

  m_VulkanCheck = NULL;
  m_VulkanInstall = NULL;

  m_TargetControlThreadShutdown = false;
  m_ControlClientThreadShutdown = false;

  ClearTrackedFiles();
}

void NoobDawn::Initialise()
{
  Callstack::Init();

  Network::Init();

  Threading::Init();

#if !NOOBDAWN_STABLE_BUILD
  Superluminal::Init();
#endif

  m_RemoteIdent = 0;
  m_RemoteThread = 0;

  m_TimeBase = 0;
  m_TimeFrequency = 1.0;

  if(!IsReplayApp())
  {
    m_TimeBase = Timing::GetTick();
    m_TimeFrequency = Timing::GetTickFrequency() / 1000.0;

    Process::ApplyEnvironmentModification();

    uint32_t port = NoobDawn_FirstTargetControlPort;

    Network::Socket *sock = Network::CreateServerSocket("0.0.0.0", port & 0xffff, 4);

    while(sock == NULL)
    {
      port++;
      if(port > NoobDawn_LastTargetControlPort)
      {
        m_RemoteIdent = 0;
        break;
      }

      sock = Network::CreateServerSocket("0.0.0.0", port & 0xffff, 4);
    }

    if(sock)
    {
      m_RemoteIdent = port;

      m_TargetControlThreadShutdown = false;
      m_RemoteThread = Threading::CreateThread([sock]() { TargetControlServerThread(sock); });

      NBDLOG("Listening for target control on %u", port);
    }
    else
    {
      NBDWARN("Couldn't open socket for target control");
    }
  }

  // set default capture log - useful for when hooks aren't setup
  // through the UI (and a log file isn't set manually)
  {
    nbdstr capture_filename;

    const nbdstr base = IsReplayApp() ? "NoobDawn" : "NoobDawn_app";

    FileIO::GetDefaultFiles(base, capture_filename, m_LoggingFilename, m_Target);

    if(m_CaptureFileTemplate.empty())
      SetCaptureFileTemplate(capture_filename);

    NBDLOGFILE(m_LoggingFilename.c_str());
  }

  const char *platform =
#if ENABLED(RDOC_WIN32)
      "Windows";
#elif ENABLED(RDOC_LINUX)
      "Linux";
#elif ENABLED(RDOC_ANDROID)
      "Android";
#elif ENABLED(RDOC_APPLE)
      "macOS";
#else
      "Unknown";
#endif

  NBDLOG("NoobDawn v%s %s %s %s (%s) %s", MAJOR_MINOR_VERSION_STRING, platform,
         sizeof(uintptr_t) == sizeof(uint64_t) ? "64-bit" : "32-bit",
         ENABLED(RDOC_RELEASE) ? "Release" : "Development", GitVersionHash,
         IsReplayApp() ? "loaded in replay application" : "capturing application");

#if defined(DISTRIBUTION_VERSION)
  NBDLOG("Packaged for %s (%s) - %s", DISTRIBUTION_NAME, DISTRIBUTION_VERSION, DISTRIBUTION_CONTACT);
#endif

#if defined(NOOBDAWN_HOOK_DLSYM)
  NBDWARN("dlsym() hooking enabled!");
#endif

  if(!IsReplayApp())
  {
    if(m_RemoteIdent == 0)
      NBDWARN("Couldn't open socket for target control");
    else
      NBDDEBUG("Listening for target control on %u", m_RemoteIdent);
  }

  Keyboard::Init();

  m_FrameTimer.InitTimers();

  m_ExHandler = NULL;

  ClearTrackedFiles();

  RecreateCrashHandler();

  // begin printing to stdout/stderr after this point, earlier logging is debugging
  // cruft that we don't want cluttering output.
  // However we don't want to print in captured applications, since they may be outputting important
  // information to stdout/stderr and being piped around and processed!
  if(IsReplayApp())
    NBDLOGOUTPUT();

  ProcessConfig();
}

NoobDawn::~NoobDawn()
{
  if(m_ExHandler)
  {
    UnloadCrashHandler();
  }

  for(auto it = m_ShutdownFunctions.begin(); it != m_ShutdownFunctions.end(); ++it)
    (*it)();
  m_ShutdownFunctions.clear();

  for(size_t i = 0; i < m_Captures.size(); i++)
  {
    if(m_Captures[i].retrieved)
    {
      NBDLOG("Removing remotely retrieved capture %s", m_Captures[i].path.c_str());
      FileIO::Delete(m_Captures[i].path);
    }
    else
    {
      NBDLOG("'Leaking' unretrieved capture %s", m_Captures[i].path.c_str());
    }
  }

  NBDSTOPLOGGING();

  if(m_RemoteThread)
  {
    m_TargetControlThreadShutdown = true;
    // On windows we can't join to this thread as it could lead to deadlocks, since we're
    // performing this destructor in the middle of module unloading. However we want to
    // ensure that the thread gets properly tidied up and closes its socket, so wait a little
    // while to give it time to notice the shutdown signal and close itself.
    Threading::Sleep(50);
    Threading::CloseThread(m_RemoteThread);
    m_RemoteThread = 0;
  }

  delete m_Config;

  Process::Shutdown();

  Network::Shutdown();

  Threading::Shutdown();

  StringFormat::Shutdown();
}

void NoobDawn::RemoveHooks()
{
  if(m_ExHandler)
  {
    UnloadCrashHandler();
  }

  if(m_RemoteThread)
  {
    // explicitly wait for thread to shutdown, this call is not from module unloading and
    // we want to be sure everything is gone before we remove our module & hooks
    m_TargetControlThreadShutdown = true;
    Threading::JoinThread(m_RemoteThread);
    Threading::CloseThread(m_RemoteThread);
    m_RemoteThread = 0;
  }
}

void NoobDawn::InitialiseReplay(GlobalEnvironment env, const nbdarray<nbdstr> &args)
{
  if(!IsReplayApp())
  {
    NBDERR(
        "Initialising replay within non-replaying app. Did you properly export replay marker in "
        "host executable or library, or are you trying to replay directly with a self-hosted "
        "capture build?");
  }

  m_GlobalEnv = env;

#if ENABLED(RDOC_LINUX) && ENABLED(RDOC_XLIB)
  if(!m_GlobalEnv.xlibDisplay)
    m_GlobalEnv.xlibDisplay = XOpenDisplay(NULL);
#endif

  nbdstr exename;
  FileIO::GetExecutableFilename(exename);
  NBDLOG("Replay application '%s' launched", exename.c_str());
  if(!args.empty())
  {
    for(size_t i = 0; i < args.size(); i++)
      NBDLOG("Parameter [%u]: %s", (uint32_t)i, args[i].c_str());
  }

  if(args.contains("--crash"))
    UnloadCrashHandler();
  else
    RecreateCrashHandler();

  if(env.enumerateGPUs)
  {
    m_AvailableGPUThread = Threading::CreateThread([this]() {
      nbdarray<nbdstr> driverFilePaths;

      for(GraphicsAPI api : {GraphicsAPI::D3D11, GraphicsAPI::D3D12, GraphicsAPI::Vulkan})
      {
        NBDDriver driverType = NBDDriver::Unknown;

        switch(api)
        {
          case GraphicsAPI::D3D11: driverType = NBDDriver::D3D11; break;
          case GraphicsAPI::D3D12: driverType = NBDDriver::D3D12; break;
          case GraphicsAPI::OpenGL: break;
          case GraphicsAPI::Vulkan: driverType = NBDDriver::Vulkan; break;
        }

        if(driverType == NBDDriver::Unknown || !HasReplayDriver(driverType))
          continue;

        IReplayDriver *driver = NULL;
        RDResult result = m_ReplayDriverProviders[driverType](NULL, ReplayOptions(), &driver);

        if(result == ResultCode::Succeeded)
        {
          nbdarray<GPUDevice> gpus = driver->GetAvailableGPUs();

#if ENABLED(RDOC_WIN32)
          for(const char *driverDLL : {
                  "amdvlk32.dll",     "amdvlk64.dll",     "atiadlxx.dll",    "atig6pxx.dll",
                  "atig6txx.dll",     "atigktxx.dll",     "atiglpxx.dll",    "atimuixx.dll",
                  "atio6axx.dll",     "atioglxx.dll",     "ControlLib.dll",  "ControlLib32.dll",
                  "igd10iumd32.dll",  "igd10iumd64.dll",  "igd12umd32.dll",  "igd12umd64.dll",
                  "igc32.dll",        "igc64.dll",        "igvk32.dll",      "igvk64.dll",
                  "igxelpgicd32.dll", "igxelpgicd64.dll", "igxelpicd32.dll", "igxelpicd32.dll",
                  "igxelpicd64.dll",  "igxelpicd64.dll",  "nvoglv32.dll",    "nvoglv64.dll",
                  "nvwgf2um.dll",     "nvwgf2umx.dll",
              })
          {
            HMODULE mod = GetModuleHandleA(driverDLL);

            if(mod)
            {
              wchar_t curFile[512] = {0};
              GetModuleFileNameW(mod, curFile, 511);

              nbdstr path = StringFormat::Wide2UTF8(curFile);

              if(!driverFilePaths.contains(path))
                driverFilePaths.push_back(path);
            }
          }
#endif

          for(const GPUDevice &newgpu : gpus)
          {
            bool addnew = true;

            for(GPUDevice &oldgpu : m_AvailableGPUs)
            {
              // if we have this GPU listed already, just add its API to the previous list
              if(oldgpu == newgpu)
              {
                oldgpu.apis.push_back(api);
                addnew = false;
              }
            }

            if(addnew)
              m_AvailableGPUs.push_back(newgpu);
          }
        }
        else
        {
          NBDWARN("Couldn't create proxy replay driver for %s: %s", ToStr(driverType).c_str(),
                  ResultDetails(result).Message().c_str());
        }

        if(driver)
          driver->Shutdown();
      }

      // we now have a list of GPUs, however we might have some duplicates if some APIs have
      // multiple drivers for a single device. To compact this list, for each GPU with no driver
      // we find all matching multi-drive GPUs and merge it into all matching copies.
      bool hasDriverNames = false;
      for(size_t i = 0; i < m_AvailableGPUs.size(); i++)
        hasDriverNames |= !m_AvailableGPUs[i].driver.empty();

      if(hasDriverNames)
      {
        for(size_t i = 0; i < m_AvailableGPUs.size();)
        {
          bool applied = false;

          if(!m_AvailableGPUs[i].driver.empty())
          {
            i++;
            continue;
          }

          // scan all subsequent GPUs, if we find a duplicate, merge the APIs
          for(size_t j = i + 1; j < m_AvailableGPUs.size(); j++)
          {
            if(m_AvailableGPUs[i].vendor == m_AvailableGPUs[j].vendor &&
               m_AvailableGPUs[i].deviceID == m_AvailableGPUs[j].deviceID)
            {
              NBDASSERT(!m_AvailableGPUs[j].driver.empty());
              for(GraphicsAPI a : m_AvailableGPUs[i].apis)
              {
                if(m_AvailableGPUs[j].apis.indexOf(a) == -1)
                  m_AvailableGPUs[j].apis.push_back(a);
              }
              applied = true;
            }
          }

          // we "applied" this GPU to all its driver-based duplicates, so we can remove it now
          if(applied)
          {
            m_AvailableGPUs.erase(i);
          }
          else
          {
            i++;
          }
        }
      }

      // sort the APIs list in each GPU, and sort the GPUs
      std::sort(m_AvailableGPUs.begin(), m_AvailableGPUs.end());
      for(GPUDevice &dev : m_AvailableGPUs)
      {
        std::sort(dev.apis.begin(), dev.apis.end());

        NBDLOG("GPU: %s - %s (driver: %s)", ToStr(dev.vendor).c_str(), dev.name.c_str(),
               dev.driver.empty() ? "--" : dev.driver.c_str());
      }

#if ENABLED(RDOC_WIN32)
      {
        // only print unique versions to avoid the case of loading multiple driver files and
        // printing redundantly.
        nbdarray<OSUtility::DLLFileVersion> versions;

        for(nbdstr &path : driverFilePaths)
        {
          OSUtility::DLLFileVersion ret = OSUtility::GetDLLVersion(path);

          if(!versions.contains(ret))
          {
            versions.push_back(ret);
            NBDLOG("Driver: '%s' %u.%u.%u.%u %s", path.c_str(), ret.major, ret.minor, ret.build,
                   ret.revision,
                   StringFormat::sntimef(FileIO::GetModifiedTimestamp(path), "%Y-%m-%d").c_str());
          }
        }
      }
#endif
    });
  }
}

void NoobDawn::ShutdownReplay()
{
  SyncAvailableGPUThread();

  // call shutdown functions early, as we only want to do these in the NoobDawn destructor if we
  // have no other choice (i.e. we're capturing).
  for(auto it = m_ShutdownFunctions.begin(); it != m_ShutdownFunctions.end(); ++it)
    (*it)();
  m_ShutdownFunctions.clear();
}

void NoobDawn::RegisterShutdownFunction(ShutdownFunction func)
{
  auto it = std::lower_bound(m_ShutdownFunctions.begin(), m_ShutdownFunctions.end(), func);
  if(it == m_ShutdownFunctions.end() || *it != func)
    m_ShutdownFunctions.insert(it - m_ShutdownFunctions.begin(), func);
}

bool NoobDawn::MatchClosestWindow(DeviceOwnedWindow &devWnd)
{
  SCOPED_LOCK(m_CapturerListLock);

  // lower_bound and the DeviceWnd ordering (pointer compares, dev over wnd) means that if either
  // element in devWnd is NULL we can go forward from this iterator and find the first wildcardMatch
  // note that if dev is specified and wnd is NULL, this will actually point at the first
  // wildcardMatch already and we can use it immediately (since which window of multiple we
  // choose is undefined, so up to us). If dev is NULL there is no window ordering (since dev is
  // the primary sorting value) so we just iterate through the whole map. It should be small in
  // the majority of cases
  auto it = m_WindowFrameCapturers.lower_bound(devWnd);

  while(it != m_WindowFrameCapturers.end())
  {
    if(it->first.wildcardMatch(devWnd))
      break;
    ++it;
  }

  if(it != m_WindowFrameCapturers.end())
  {
    devWnd = it->first;
    return true;
  }

  return false;
}

bool NoobDawn::IsActiveWindow(DeviceOwnedWindow devWnd)
{
  SCOPED_LOCK(m_CapturerListLock);
  return devWnd == m_ActiveWindow;
}

void NoobDawn::GetActiveWindow(DeviceOwnedWindow &devWnd)
{
  SCOPED_LOCK(m_CapturerListLock);
  devWnd = m_ActiveWindow;
}

IFrameCapturer *NoobDawn::MatchFrameCapturer(DeviceOwnedWindow devWnd)
{
  // try and find the closest frame capture registered, and update
  // the values in devWnd to point to it precisely
  bool exactMatch = MatchClosestWindow(devWnd);

  SCOPED_LOCK(m_CapturerListLock);

  if(!exactMatch)
  {
    // handle off-screen rendering where there are no device/window pairs in
    // m_WindowFrameCapturers, instead we use the first matching device frame capturer
    if(devWnd.windowHandle == NULL)
    {
      auto defaultit = m_DeviceFrameCapturers.find(devWnd.device);
      if(defaultit == m_DeviceFrameCapturers.end() && !m_DeviceFrameCapturers.empty())
        defaultit = m_DeviceFrameCapturers.begin();

      if(defaultit != m_DeviceFrameCapturers.end())
        return defaultit->second;
    }

    NBDERR(
        "Couldn't find matching frame capturer for device %p window %p "
        "from %zu device frame capturers and %zu frame capturers",
        devWnd.device, devWnd.windowHandle, m_DeviceFrameCapturers.size(),
        m_WindowFrameCapturers.size());
    return NULL;
  }

  auto it = m_WindowFrameCapturers.find(devWnd);

  if(it == m_WindowFrameCapturers.end())
  {
    NBDERR("Couldn't find frame capturer after exact match!");
    return NULL;
  }

  return it->second.FrameCapturer;
}

void NoobDawn::StartFrameCapture(DeviceOwnedWindow devWnd)
{
  m_CaptureTitle.clear();
  IFrameCapturer *frameCap = MatchFrameCapturer(devWnd);
  if(frameCap)
  {
    frameCap->StartFrameCapture(devWnd);
    m_CapturesActive++;
  }
}

void NoobDawn::SetActiveWindow(DeviceOwnedWindow devWnd)
{
  SCOPED_LOCK(m_CapturerListLock);

  auto it = m_WindowFrameCapturers.find(devWnd);
  if(it == m_WindowFrameCapturers.end())
  {
    NBDERR("Couldn't find frame capturer for device %p window %p", devWnd.device,
           devWnd.windowHandle);
    return;
  }

  m_ActiveWindow = devWnd;
}

void NoobDawn::SetCaptureTitle(const nbdstr &title)
{
  m_CaptureTitle = title;
}

bool NoobDawn::EndFrameCapture(DeviceOwnedWindow devWnd)
{
  IFrameCapturer *frameCap = MatchFrameCapturer(devWnd);
  if(frameCap)
  {
    bool ret = frameCap->EndFrameCapture(devWnd);
    m_CapturesActive--;
    return ret;
  }
  return false;
}

bool NoobDawn::DiscardFrameCapture(DeviceOwnedWindow devWnd)
{
  IFrameCapturer *frameCap = MatchFrameCapturer(devWnd);
  if(frameCap)
  {
    bool ret = frameCap->DiscardFrameCapture(devWnd);
    m_CapturesActive--;
    return ret;
  }
  return false;
}

bool NoobDawn::IsTargetControlConnected()
{
  SCOPED_LOCK(m_SingleClientLock);
  return !m_SingleClientName.empty();
}

nbdstr NoobDawn::GetTargetControlUsername()
{
  SCOPED_LOCK(m_SingleClientLock);
  return m_SingleClientName;
}

bool NoobDawn::ShowReplayUI()
{
  SCOPED_LOCK(m_SingleClientLock);
  if(m_SingleClientName.empty())
    return false;

  m_RequestControllerShow = true;
  return true;
}

void NoobDawn::Tick()
{
  bool cur_focus = false;
  for(size_t i = 0; i < m_FocusKeys.size(); i++)
    cur_focus |= Keyboard::GetKeyState(m_FocusKeys[i]);

  bool cur_cap = false;
  for(size_t i = 0; i < m_CaptureKeys.size(); i++)
    cur_cap |= Keyboard::GetKeyState(m_CaptureKeys[i]);

  m_FrameTimer.UpdateTimers();

  if(!m_PrevFocus && cur_focus)
  {
    CycleActiveWindow();
  }
  if(!m_PrevCap && cur_cap)
  {
    TriggerCapture(1);
  }

  m_PrevFocus = cur_focus;
  m_PrevCap = cur_cap;

  // check for any child threads that need to be waited on, remove them from the list
  nbdarray<Threading::ThreadHandle> waitThreads;
  {
    SCOPED_LOCK(m_ChildLock);
    for(nbdpair<uint32_t, Threading::ThreadHandle> &c : m_ChildThreads)
    {
      if(c.first == 0)
        waitThreads.push_back(c.second);
    }

    m_ChildThreads.removeIf(
        [](const nbdpair<uint32_t, Threading::ThreadHandle> &c) { return c.first == 0; });
  }

  // clean up the threads now
  for(Threading::ThreadHandle t : waitThreads)
  {
    Threading::JoinThread(t);
    Threading::CloseThread(t);
  }
}

void NoobDawn::CycleActiveWindow()
{
  SCOPED_LOCK(m_CapturerListLock);

  m_Cap = 0;

  // can only shift focus if we have multiple windows
  if(m_WindowFrameCapturers.size() > 1)
  {
    for(auto it = m_WindowFrameCapturers.begin(); it != m_WindowFrameCapturers.end(); ++it)
    {
      if(it->first == m_ActiveWindow)
      {
        auto nextit = it;
        ++nextit;

        if(nextit != m_WindowFrameCapturers.end())
          m_ActiveWindow = nextit->first;
        else
          m_ActiveWindow = m_WindowFrameCapturers.begin()->first;

        break;
      }
    }
  }
}

uint32_t NoobDawn::GetCapturableWindowCount()
{
  SCOPED_LOCK(m_CapturerListLock);
  return (uint32_t)m_WindowFrameCapturers.size();
}

nbdstr NoobDawn::GetOverlayText(NBDDriver driver, DeviceOwnedWindow devWnd, uint32_t frameNumber,
                                 int flags)
{
  bool activeWindow;
  const bool capturesEnabled = (flags & eOverlay_CaptureDisabled) == 0;

  uint32_t overlay = GetOverlayBits();

  NBDDriver activeDriver = NBDDriver::Unknown;
  NBDDriver curDriver = NBDDriver::Unknown;

  int activeIdx = -1, curIdx = -1, idx = 0;
  size_t numWindows;
  {
    SCOPED_LOCK(m_CapturerListLock);

    activeWindow = (devWnd == m_ActiveWindow);

    for(auto it = m_WindowFrameCapturers.begin(); it != m_WindowFrameCapturers.end(); ++it, ++idx)
    {
      if(it->first == m_ActiveWindow)
      {
        activeIdx = idx;
        activeDriver = it->second.FrameCapturer->GetFrameCaptureDriver();
      }
      if(it->first == devWnd)
      {
        curIdx = idx;
        curDriver = it->second.FrameCapturer->GetFrameCaptureDriver();
      }
    }

    numWindows = m_WindowFrameCapturers.size();
  }

  if(activeDriver == NBDDriver::Unknown)
    activeDriver = curDriver;

  if(activeDriver == NBDDriver::Unknown)
    activeDriver = driver;

  // example layout:
  //
  // Capturing D3D11.  Frame: 1234. 33ms (30 FPS)
  // F12, PrtScrn to capture. 3 Captures saved.
  // Captured frame 1200.
  //
  // Frame number, FPS, capture list are optional. If capture list is disabled
  // the second line still displays the keys as long as capturing is allowed.
  // if capturing is disabled, only the first line displays.
  //
  // On platforms without keyboards, the keys are replaced by a remote access connection status
  // message.
  //
  // with multiple windows the active window will look like:
  //
  // Capturing D3D11.  Window 1 active. Frame: 1234. 33ms (30 FPS)
  // F12, PrtScrn to capture. 3 Captures saved.
  // Captured frame 1200.
  //
  // Inactive windows will look like:
  //
  // Capturing D3D11.  Window 1 active.
  // F11 to cycle. OpenGL window 2.

  nbdstr overlayText = ToStr(activeDriver) + ".";

  // pad this so it's the same length regardless of API length
  while(overlayText.length() < 8)
    overlayText.push_back(' ');

  overlayText = "Capturing " + overlayText;

  if(numWindows > 1)
  {
    if(activeIdx >= 0)
      overlayText += StringFormat::Fmt(" Window %d active.", activeIdx);
    else
      overlayText += " No window active.";
  }

  if(activeWindow)
  {
    if(overlay & eNOOBDAWN_Overlay_FrameNumber)
      overlayText += StringFormat::Fmt(" Frame: %d.", frameNumber);

    if(overlay & eNOOBDAWN_Overlay_FrameRate)
    {
      const double frameTime = m_FrameTimer.GetAvgFrameTime();
      // max with 0.01ms so that we don't divide by zero
      const double fps = 1000.0f / NBDMAX(0.01, frameTime);

      if(frameTime < 0.0001)
      {
        overlayText += " --- ms (--- FPS)";
      }
      else
      {
        // only display frametime fractions if it's relevant (sub-integer frame time or FPS)

        if(frameTime < 1.0)
          overlayText += StringFormat::Fmt(" %.2lf ms", m_FrameTimer.GetAvgFrameTime());
        else
          overlayText += StringFormat::Fmt(" %d ms", int(m_FrameTimer.GetAvgFrameTime()));

        if(fps < 1.0)
          overlayText += StringFormat::Fmt(" (%.2lf FPS)", fps);
        else
          overlayText += StringFormat::Fmt(" (%d FPS)", int(fps));
      }
    }
  }

  overlayText += "\n";

#if ENABLED(RDOC_DEVEL)
  {
    overlayText += StringFormat::Fmt("%llu chunks - %.2f MB\n", Chunk::NumLiveChunks(),
                                     float(Chunk::TotalMem()) / 1024.0f / 1024.0f);
  }
#endif

  if(capturesEnabled)
  {
    if(activeWindow)
    {
      nbdarray<NOOBDAWN_InputButton> keys = GetCaptureKeys();

      if(Keyboard::PlatformHasKeyInput())
      {
        for(size_t i = 0; i < keys.size(); i++)
        {
          if(i > 0)
            overlayText += ", ";

          overlayText += ToStr(keys[i]);
        }

        if(!keys.empty())
          overlayText += " to capture.";
      }
      else
      {
        if(IsTargetControlConnected())
          overlayText += "Connected by " + GetTargetControlUsername() + ".";
        else
          overlayText += "No remote access connection.";
      }

      if(overlay & eNOOBDAWN_Overlay_CaptureList)
      {
        overlayText += StringFormat::Fmt(" %d Captures saved.\n", (uint32_t)m_Captures.size());

        uint64_t now = Timing::GetUnixTimestamp();
        for(size_t i = 0; i < m_Captures.size(); i++)
        {
          if(now - m_Captures[i].timestamp < 20)
          {
            if(m_Captures[i].frameNumber == ~0U)
              overlayText += "Captured user-defined capture.\n";
            else
              overlayText += StringFormat::Fmt("Captured frame %d.\n", m_Captures[i].frameNumber);
          }
        }
      }
    }
    else
    {
      nbdarray<NOOBDAWN_InputButton> keys = GetFocusKeys();

      if(Keyboard::PlatformHasKeyInput())
      {
        for(size_t i = 0; i < keys.size(); i++)
        {
          if(i > 0)
            overlayText += ", ";

          overlayText += ToStr(keys[i]);
        }

        if(!keys.empty())
          overlayText += " to cycle.";
      }
      else
      {
        if(IsTargetControlConnected())
          overlayText += "Connected by " + GetTargetControlUsername() + ".";
        else
          overlayText += "No remote access connection.";
      }

      if(curIdx >= 0)
        overlayText += StringFormat::Fmt(" %s window %d.", ToStr(curDriver).c_str(), curIdx);
      else if(curDriver != NBDDriver::Unknown)
        overlayText += StringFormat::Fmt(" Unknown %s window.", ToStr(curDriver).c_str());
      else
        overlayText += " Unknown window.";
    }
  }

  return overlayText;
}

void NoobDawn::QueueCapture(uint32_t frameNumber)
{
  auto it = std::lower_bound(m_QueuedFrameCaptures.begin(), m_QueuedFrameCaptures.end(), frameNumber);
  if(it == m_QueuedFrameCaptures.end() || *it != frameNumber)
    m_QueuedFrameCaptures.insert(it - m_QueuedFrameCaptures.begin(), frameNumber);
}

bool NoobDawn::ShouldTriggerCapture(uint32_t frameNumber)
{
  bool ret = m_Cap > 0;

  if(m_Cap > 0)
    m_Cap--;

  nbdarray<uint32_t> frames;
  frames.swap(m_QueuedFrameCaptures);
  for(auto it = frames.begin(); it != frames.end(); ++it)
  {
    if(*it < frameNumber)
    {
      // discard, this frame is past.
    }
    else if((*it) == frameNumber)
    {
      // we want to capture the next frame
      ret = true;
    }
    else
    {
      // not hit this yet, keep it around
      m_QueuedFrameCaptures.push_back(*it);
    }
  }

  return ret;
}

void NoobDawn::ResamplePixels(const FramePixels &in, NBDThumb &out)
{
  if(in.width == 0 || in.height == 0)
  {
    out = NBDThumb();
    return;
  }

  // code below assumes pitch_requirement is a power of 2 number
  NBDASSERT((in.pitch_requirement & (in.pitch_requirement - 1)) == 0);

  out.width = (uint16_t)NBDMIN(in.max_width, in.width);
  out.width &= ~(in.pitch_requirement - 1);    // align down to multiple of in.
  out.height = uint16_t(out.width * in.height / in.width);
  out.pixels.resize(3 * out.width * out.height);
  out.format = FileType::Raw;

  byte *dst = (byte *)out.pixels.data();
  byte *source = (byte *)in.data;

  for(uint32_t y = 0; y < out.height; y++)
  {
    for(uint32_t x = 0; x < out.width; x++)
    {
      uint32_t xSource = x * in.width / out.width;
      uint32_t ySource = y * in.height / out.height;
      byte *src = &source[in.stride * xSource + in.pitch * ySource];

      if(in.buf1010102)
      {
        uint32_t *src1010102 = (uint32_t *)src;
        Vec4f unorm = ConvertFromR10G10B10A2(*src1010102);
        dst[0] = (byte)(unorm.x * 255.0f);
        dst[1] = (byte)(unorm.y * 255.0f);
        dst[2] = (byte)(unorm.z * 255.0f);
      }
      else if(in.buf565)
      {
        uint16_t *src565 = (uint16_t *)src;
        Vec3f unorm = ConvertFromB5G6R5(*src565);
        dst[0] = (byte)(unorm.x * 255.0f);
        dst[1] = (byte)(unorm.y * 255.0f);
        dst[2] = (byte)(unorm.z * 255.0f);
      }
      else if(in.buf5551)
      {
        uint16_t *src5551 = (uint16_t *)src;
        Vec4f unorm = ConvertFromB5G5R5A1(*src5551);
        dst[0] = (byte)(unorm.x * 255.0f);
        dst[1] = (byte)(unorm.y * 255.0f);
        dst[2] = (byte)(unorm.z * 255.0f);
      }
      else if(in.bgra)
      {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
      }
      else if(in.bpc == 2)    // R16G16B16A16 backbuffer
      {
        uint16_t *src16 = (uint16_t *)src;

        float linearR = NBDCLAMP(ConvertFromHalf(src16[0]), 0.0f, 1.0f);
        float linearG = NBDCLAMP(ConvertFromHalf(src16[1]), 0.0f, 1.0f);
        float linearB = NBDCLAMP(ConvertFromHalf(src16[2]), 0.0f, 1.0f);

        dst[0] = byte(255.0f * ConvertLinearToSRGB(linearR));
        dst[1] = byte(255.0f * ConvertLinearToSRGB(linearG));
        dst[2] = byte(255.0f * ConvertLinearToSRGB(linearB));
      }
      else
      {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
      }
      dst += 3;
    }
  }

  if(!in.is_y_flipped)
  {
    for(uint16_t y = 0; y < out.height / 2; y++)
    {
      uint16_t flipY = (out.height - 1 - y);
      for(uint16_t x = 0; x < out.width; x++)
      {
        byte *src = (byte *)out.pixels.data();
        byte save[3];
        save[0] = src[(y * out.width + x) * 3 + 0];
        save[1] = src[(y * out.width + x) * 3 + 1];
        save[2] = src[(y * out.width + x) * 3 + 2];

        src[(y * out.width + x) * 3 + 0] = src[(flipY * out.width + x) * 3 + 0];
        src[(y * out.width + x) * 3 + 1] = src[(flipY * out.width + x) * 3 + 1];
        src[(y * out.width + x) * 3 + 2] = src[(flipY * out.width + x) * 3 + 2];

        src[(flipY * out.width + x) * 3 + 0] = save[0];
        src[(flipY * out.width + x) * 3 + 1] = save[1];
        src[(flipY * out.width + x) * 3 + 2] = save[2];
      }
    }
  }
}

void NoobDawn::EncodeThumbPixels(const NBDThumb &in, NBDThumb &out)
{
  if(in.width == 0 || in.height == 0)
  {
    out = NBDThumb();
    return;
  }

  struct WriteCallbackData
  {
    bytebuf buffer;

    static void writeData(void *context, void *data, int size)
    {
      WriteCallbackData *pThis = (WriteCallbackData *)context;
      pThis->buffer.append((const byte *)data, size);
    }
  };

  if(out.format == FileType::PNG)
  {
    WriteCallbackData callbackData;
    stbi_write_png_to_func(&WriteCallbackData::writeData, &callbackData, in.width, in.height, 3,
                           in.pixels.data(), 0);
    out.width = in.width;
    out.height = in.height;
    out.pixels.swap(callbackData.buffer);
  }
  else
  {
    // should be JPG if not PNG
    NBDASSERTEQUAL(out.format, FileType::JPG);

    out.width = in.width;
    out.height = in.height;
    out.format = FileType::JPG;

    const int comp = 3;
    int len = in.width * in.height * comp;
    out.pixels.resize(len);
    jpge::params p;
    p.m_quality = 90;
    jpge::compress_image_to_jpeg_file_in_memory(out.pixels.data(), len, in.width, in.height, comp,
                                                in.pixels.data(), p);
    out.pixels.resize(len);
  }
}

NBDFile *NoobDawn::CreateNBD(NBDDriver driver, uint32_t frameNum, const FramePixels &fp)
{
  NBDFile *ret = new NBDFile;

  nbdstr suffix = StringFormat::Fmt("_frame%u", frameNum);

  if(frameNum == ~0U)
    suffix = "_capture";

  m_CurrentLogFile = StringFormat::Fmt("%s%s.nbd", m_CaptureFileTemplate.c_str(), suffix.c_str());

  // make sure we don't stomp another capture if we make multiple captures in the same frame.
  {
    SCOPED_LOCK(m_CaptureLock);
    int altnum = 2;
    while(std::find_if(m_Captures.begin(), m_Captures.end(), [this](const CaptureData &o) {
            return o.path == m_CurrentLogFile;
          }) != m_Captures.end())
    {
      m_CurrentLogFile =
          StringFormat::Fmt("%s%s_%d.nbd", m_CaptureFileTemplate.c_str(), suffix.c_str(), altnum);
      altnum++;
    }
  }

  NBDThumb outRaw, outThumb;
  if(fp.data)
  {
    // point sample info into raw buffer
    ResamplePixels(fp, outRaw);

    if(Capture_IncludeExtendedThumbnail())
      outThumb.format = FileType::PNG;
    else
      outThumb.format = FileType::JPG;

    EncodeThumbPixels(outRaw, outThumb);
  }

  ret->SetData(driver, ToStr(driver).c_str(), OSUtility::GetMachineIdent(), &outThumb, m_TimeBase,
               m_TimeFrequency);

  FileIO::CreateParentDirectory(m_CurrentLogFile);

  ret->Create(m_CurrentLogFile.c_str());

  if(ret->Error() != ResultCode::Succeeded)
    SAFE_DELETE(ret);

  return ret;
}

bool NoobDawn::HasReplayDriver(NBDDriver driver) const
{
  // Image driver is handled specially and isn't registered in the map
  if(driver == NBDDriver::Image)
    return true;

  return m_ReplayDriverProviders.find(driver) != m_ReplayDriverProviders.end();
}

bool NoobDawn::HasRemoteDriver(NBDDriver driver) const
{
  if(m_RemoteDriverProviders.find(driver) != m_RemoteDriverProviders.end())
    return true;

  return HasReplayDriver(driver);
}

void NoobDawn::RegisterReplayProvider(NBDDriver driver, ReplayDriverProvider provider)
{
  if(HasReplayDriver(driver))
    NBDERR("Re-registering provider for %s", ToStr(driver).c_str());
  if(HasRemoteDriver(driver))
    NBDWARN("Registering local provider for existing remote provider %s", ToStr(driver).c_str());

  m_ReplayDriverProviders[driver] = provider;
}

void NoobDawn::RegisterRemoteProvider(NBDDriver driver, RemoteDriverProvider provider)
{
  if(HasRemoteDriver(driver))
    NBDERR("Re-registering provider for %s", ToStr(driver).c_str());
  if(HasReplayDriver(driver))
    NBDWARN("Registering remote provider for existing local provider %s", ToStr(driver).c_str());

  m_RemoteDriverProviders[driver] = provider;
}

void NoobDawn::RegisterStructuredProcessor(NBDDriver driver, StructuredProcessor provider)
{
  NBDASSERT(m_StructProcesssors.find(driver) == m_StructProcesssors.end());

  m_StructProcesssors[driver] = provider;
}

void NoobDawn::RegisterCaptureExporter(CaptureExporter exporter, CaptureFileFormat description)
{
  nbdstr filetype = description.extension;

  for(const CaptureFileFormat &fmt : m_ImportExportFormats)
  {
    if(fmt.extension == filetype)
    {
      NBDERR("Duplicate exporter for '%s' found", filetype.c_str());
      return;
    }
  }

  description.openSupported = false;
  description.convertSupported = true;

  m_ImportExportFormats.push_back(description);

  m_Exporters[filetype] = exporter;
}

void NoobDawn::RegisterCaptureImportExporter(CaptureImporter importer, CaptureExporter exporter,
                                              CaptureFileFormat description)
{
  nbdstr filetype = description.extension;

  for(const CaptureFileFormat &fmt : m_ImportExportFormats)
  {
    if(fmt.extension == filetype)
    {
      NBDERR("Duplicate import/exporter for '%s' found", filetype.c_str());
      return;
    }
  }

  description.openSupported = true;
  description.convertSupported = true;

  m_ImportExportFormats.push_back(description);

  m_Importers[filetype] = importer;
  m_Exporters[filetype] = exporter;
}

void NoobDawn::RegisterDeviceProtocol(const nbdstr &protocol, ProtocolHandler handler)
{
  if(m_Protocols[protocol] != NULL)
  {
    NBDERR("Duplicate protocol registration: %s", protocol.c_str());
    return;
  }
  m_Protocols[protocol] = handler;
}

StructuredProcessor NoobDawn::GetStructuredProcessor(NBDDriver driver)
{
  auto it = m_StructProcesssors.find(driver);

  if(it == m_StructProcesssors.end())
    return NULL;

  return it->second;
}

CaptureExporter NoobDawn::GetCaptureExporter(const nbdstr &filetype)
{
  auto it = m_Exporters.find(filetype);

  if(it == m_Exporters.end())
    return NULL;

  return it->second;
}

CaptureImporter NoobDawn::GetCaptureImporter(const nbdstr &filetype)
{
  auto it = m_Importers.find(filetype);

  if(it == m_Importers.end())
    return NULL;

  return it->second;
}

nbdarray<nbdstr> NoobDawn::GetSupportedDeviceProtocols()
{
  nbdarray<nbdstr> ret;

  for(auto it = m_Protocols.begin(); it != m_Protocols.end(); ++it)
    ret.push_back(it->first);

  return ret;
}

IDeviceProtocolHandler *NoobDawn::GetDeviceProtocol(const nbdstr &protocol)
{
  nbdstr p = protocol;

  // allow passing in an URL with ://
  int32_t offs = p.find("://");
  if(offs >= 0)
    p.erase(offs, p.size() - offs);

  auto it = m_Protocols.find(p);

  if(it != m_Protocols.end())
    return it->second();

  return NULL;
}

nbdarray<CaptureFileFormat> NoobDawn::GetCaptureFileFormats()
{
  nbdarray<CaptureFileFormat> ret = m_ImportExportFormats;

  std::sort(ret.begin(), ret.end());

  {
    CaptureFileFormat nbd;
    nbd.extension = "nbd";
    nbd.name = "Native NBD capture file format.";
    nbd.description = "The format produced by frame-captures from applications directly.";
    nbd.openSupported = true;
    nbd.convertSupported = true;

    ret.insert(0, nbd);
  }

  return ret;
}

nbdarray<GPUDevice> NoobDawn::GetAvailableGPUs()
{
  SyncAvailableGPUThread();

  return m_AvailableGPUs;
}

void NoobDawn::SyncAvailableGPUThread()
{
  if(m_AvailableGPUThread)
  {
    Threading::JoinThread(m_AvailableGPUThread);
    Threading::CloseThread(m_AvailableGPUThread);
    m_AvailableGPUThread = 0;
  }
}

bool NoobDawn::HasReplaySupport(NBDDriver driverType)
{
  if(driverType == NBDDriver::Image)
    return true;

  if(driverType == NBDDriver::Unknown && !m_ReplayDriverProviders.empty())
    return true;

  return m_ReplayDriverProviders.find(driverType) != m_ReplayDriverProviders.end();
}

RDResult NoobDawn::CreateProxyReplayDriver(NBDDriver proxyDriver, IReplayDriver **driver)
{
  SyncAvailableGPUThread();

  // passing NBDDriver::Unknown means 'I don't care, give me a proxy driver of any type'
  if(proxyDriver == NBDDriver::Unknown)
  {
    if(!m_ReplayDriverProviders.empty())
      return m_ReplayDriverProviders.begin()->second(NULL, ReplayOptions(), driver);
  }

  if(m_ReplayDriverProviders.find(proxyDriver) != m_ReplayDriverProviders.end())
    return m_ReplayDriverProviders[proxyDriver](NULL, ReplayOptions(), driver);

  RETURN_ERROR_RESULT(ResultCode::APIUnsupported, "Unsupported replay driver requested: %s",
                      ToStr(proxyDriver).c_str());
}

RDResult NoobDawn::CreateReplayDriver(NBDFile *nbd, const ReplayOptions &opts, IReplayDriver **driver)
{
  if(driver == NULL)
    return ResultCode::InvalidParameter;

  SyncAvailableGPUThread();

  // allows passing NULL nbdfile as 'I don't care, give me a proxy driver of any type'
  if(nbd == NULL)
  {
    if(!m_ReplayDriverProviders.empty())
      return m_ReplayDriverProviders.begin()->second(NULL, opts, driver);

    RETURN_ERROR_RESULT(ResultCode::APIUnsupported,
                        "Request for proxy replay device, but no replay providers are available.");
  }

  NBDDriver driverType = nbd->GetDriver();

  // image support is special, handle it here
  if(driverType == NBDDriver::Image)
    return IMG_CreateReplayDevice(nbd, driver);

  if(m_ReplayDriverProviders.find(driverType) != m_ReplayDriverProviders.end())
    return m_ReplayDriverProviders[driverType](nbd, opts, driver);

  NBDERR("Unsupported replay driver requested: %s", ToStr(driverType).c_str());
  return ResultCode::APIUnsupported;
}

RDResult NoobDawn::CreateRemoteDriver(NBDFile *nbd, const ReplayOptions &opts, IRemoteDriver **driver)
{
  if(nbd == NULL || driver == NULL)
    return ResultCode::InvalidParameter;

  SyncAvailableGPUThread();

  NBDDriver driverType = nbd->GetDriver();

  if(m_RemoteDriverProviders.find(driverType) != m_RemoteDriverProviders.end())
    return m_RemoteDriverProviders[driverType](nbd, opts, driver);

  // replay drivers are remote drivers, fall back and try them
  if(m_ReplayDriverProviders.find(driverType) != m_ReplayDriverProviders.end())
  {
    IReplayDriver *dr = NULL;
    RDResult result = m_ReplayDriverProviders[driverType](nbd, opts, &dr);

    if(result == ResultCode::Succeeded)
      *driver = (IRemoteDriver *)dr;
    else
      NBDASSERT(dr == NULL);

    return result;
  }

  RETURN_ERROR_RESULT(ResultCode::APIUnsupported, "Unsupported replay driver requested: %s",
                      ToStr(driverType).c_str());
}

void NoobDawn::AddActiveDriver(NBDDriver driver, bool present)
{
  if(driver == NBDDriver::Unknown)
    return;

  uint64_t timestamp = present ? Timing::GetUnixTimestamp() : 0;

  {
    SCOPED_LOCK(m_DriverLock);

    uint64_t &active = m_ActiveDrivers[driver];
    active = NBDMAX(active, timestamp);
  }
}

void NoobDawn::SetDriverUnsupportedMessage(NBDDriver driver, nbdstr message)
{
  if(driver == NBDDriver::Unknown)
    return;

  SCOPED_LOCK(m_DriverLock);
  m_APISupportMessages[driver] = message;
}

std::map<NBDDriver, NBDDriverStatus> NoobDawn::GetActiveDrivers()
{
  std::map<NBDDriver, uint64_t> drivers;

  {
    SCOPED_LOCK(m_DriverLock);
    drivers = m_ActiveDrivers;
  }

  std::map<NBDDriver, NBDDriverStatus> ret;

  for(auto it = drivers.begin(); it != drivers.end(); ++it)
  {
    NBDDriverStatus &status = ret[it->first];
    // driver is presenting if the timestamp is greater than 0 and less than 10 seconds ago (gives a
    // little leeway for loading screens or something where the presentation stops temporarily).
    // we also assume that during a capture if it was presenting, then it's still capturing.
    // Otherwise a long capture would temporarily set it as not presenting.
    status.presenting = it->second > 0;

    if(status.presenting && !IsFrameCapturing() && it->second < Timing::GetUnixTimestamp() - 10)
      status.presenting = false;

    status.supported = (HasRemoteDriver(it->first) || HasReplayDriver(it->first)) &&
                       HasActiveFrameCapturer(it->first);

    if(!status.supported)
    {
      SCOPED_LOCK(m_DriverLock);
      status.supportMessage = m_APISupportMessages[it->first];
    }
  }

  return ret;
}

std::map<NBDDriver, nbdstr> NoobDawn::GetReplayDrivers()
{
  std::map<NBDDriver, nbdstr> ret;
  for(auto it = m_ReplayDriverProviders.begin(); it != m_ReplayDriverProviders.end(); ++it)
    ret[it->first] = ToStr(it->first);
  return ret;
}

std::map<NBDDriver, nbdstr> NoobDawn::GetRemoteDrivers()
{
  std::map<NBDDriver, nbdstr> ret;

  for(auto it = m_RemoteDriverProviders.begin(); it != m_RemoteDriverProviders.end(); ++it)
    ret[it->first] = ToStr(it->first);

  // replay drivers are remote drivers.
  for(auto it = m_ReplayDriverProviders.begin(); it != m_ReplayDriverProviders.end(); ++it)
    ret[it->first] = ToStr(it->first);

  return ret;
}

DriverInformation NoobDawn::GetDriverInformation(GraphicsAPI api)
{
  DriverInformation ret = {};

  NBDDriver driverType = NBDDriver::Unknown;
  switch(api)
  {
    case GraphicsAPI::D3D11: driverType = NBDDriver::D3D11; break;
    case GraphicsAPI::D3D12: driverType = NBDDriver::D3D12; break;
    case GraphicsAPI::OpenGL: driverType = NBDDriver::OpenGL; break;
    case GraphicsAPI::Vulkan: driverType = NBDDriver::Vulkan; break;
  }

  if(driverType == NBDDriver::Unknown || !HasReplayDriver(driverType))
    return ret;

  IReplayDriver *driver = NULL;
  RDResult result = CreateProxyReplayDriver(driverType, &driver);

  if(result == ResultCode::Succeeded)
  {
    ret = driver->GetDriverInfo();
  }
  else
  {
    NBDERR("Couldn't create proxy replay driver for %s: %s", ToStr(driverType).c_str(),
           ResultDetails(result).Message().c_str());
  }

  if(driver)
    driver->Shutdown();

  return ret;
}

void NoobDawn::EnableVendorExtensions(VendorExtensions ext)
{
  m_VendorExts[(int)ext] = true;

  NBDWARN("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  NBDWARN("!!! Vendor Extension enabled: %s", ToStr(ext).c_str());
  NBDWARN("!!! ");
  NBDWARN("!!! This can cause crashes, incorrect replay, or other problems and");
  NBDWARN("!!! is explicitly unsupported. Do not enable without understanding.");
  NBDWARN("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
}

void NoobDawn::SetCaptureOptions(const CaptureOptions &opts)
{
  m_Options = opts;

  LibraryHooks::OptionsUpdated();
}

void NoobDawn::SetCaptureFileTemplate(const nbdstr &pathtemplate)
{
  if(pathtemplate.empty())
    return;

  m_CaptureFileTemplate = pathtemplate;

  if(m_CaptureFileTemplate.length() > 4 &&
     m_CaptureFileTemplate.substr(m_CaptureFileTemplate.length() - 4) == ".nbd")
    m_CaptureFileTemplate = m_CaptureFileTemplate.substr(0, m_CaptureFileTemplate.length() - 4);

  FileIO::CreateParentDirectory(m_CaptureFileTemplate);
}

void NoobDawn::FinishCaptureWriting(NBDFile *nbd, uint32_t frameNumber)
{
  NoobDawn::Inst().SetProgress(CaptureProgress::FileWriting, 0.0f);

  if(nbd)
  {
    // add the resolve database if we were capturing callstacks.
    if(m_Options.captureCallstacks)
    {
      SectionProperties props = {};
      props.type = SectionType::ResolveDatabase;
      props.version = 1;
      StreamWriter *w = nbd->WriteSection(props);

      size_t sz = 0;
      Callstack::GetLoadedModules(NULL, sz);

      byte *buf = new byte[sz];
      Callstack::GetLoadedModules(buf, sz);

      w->Write(buf, sz);

      w->Finish();

      delete w;
    }

    const NBDThumb &thumb = nbd->GetThumbnail();
    if(thumb.format != FileType::JPG && thumb.width > 0 && thumb.height > 0)
    {
      SectionProperties props = {};
      props.type = SectionType::ExtendedThumbnail;
      props.version = 1;
      StreamWriter *w = nbd->WriteSection(props);

      // if this file format ever changes, be sure to update the XML export which has a special
      // handling for this case.

      ExtThumbnailHeader header;
      header.width = thumb.width;
      header.height = thumb.height;
      header.format = thumb.format;
      header.len = (uint32_t)thumb.pixels.size();
      w->Write(header);
      w->Write(thumb.pixels.data(), thumb.pixels.size());

      w->Finish();

      delete w;
    }

    if(Capture_Debug_SnapshotDiagnosticLog())
    {
      nbdstr logcontents = FileIO::logfile_readall(0, NBDGETLOGFILE());

      SectionProperties props = {};
      props.type = SectionType::EmbeddedLogfile;
      props.version = 1;
      props.flags = SectionFlags::LZ4Compressed;
      StreamWriter *w = nbd->WriteSection(props);

      w->Write(logcontents.data(), logcontents.size());

      w->Finish();

      delete w;
    }

    NBDLOG("Written to disk: %s", m_CurrentLogFile.c_str());

    CaptureData cap;
    cap.path = m_CurrentLogFile;
    cap.title = m_CaptureTitle;
    cap.timestamp = Timing::GetUnixTimestamp();
    cap.driver = nbd->GetDriver();
    cap.frameNumber = frameNumber;
    m_CaptureTitle.clear();
    {
      SCOPED_LOCK(m_CaptureLock);
      m_Captures.push_back(cap);
    }

    delete nbd;
  }
  else
  {
    NBDLOG("Discarded capture, Frame %u", frameNumber);
  }

  NoobDawn::Inst().SetProgress(CaptureProgress::FileWriting, 1.0f);
}

void NoobDawn::AddChildProcess(uint32_t pid, uint32_t ident)
{
  if(ident == 0 || ident == m_RemoteIdent)
  {
    NBDERR("Child process %u returned invalid ident %u. Possibly too many listen sockets in use!",
           pid, ident);
    return;
  }

  SCOPED_LOCK(m_ChildLock);
  m_Children.push_back(make_nbdpair(pid, ident));
}

nbdarray<nbdpair<uint32_t, uint32_t>> NoobDawn::GetChildProcesses()
{
  SCOPED_LOCK(m_ChildLock);
  return m_Children;
}

void NoobDawn::CompleteChildThread(uint32_t pid)
{
  SCOPED_LOCK(m_ChildLock);
  // the thread for this PID is done, mark it as ready to wait on by zero-ing out the PID
  for(nbdpair<uint32_t, Threading::ThreadHandle> &c : m_ChildThreads)
  {
    if(c.first == pid)
      c.first = 0;
  }
}

void NoobDawn::AddChildThread(uint32_t pid, Threading::ThreadHandle thread)
{
  SCOPED_LOCK(m_ChildLock);
  m_ChildThreads.push_back(make_nbdpair(pid, thread));
}

void NoobDawn::ValidateCaptures()
{
  SCOPED_LOCK(m_CaptureLock);
  m_Captures.removeIf([](const CaptureData &cap) { return !FileIO::exists(cap.path); });
}

nbdarray<CaptureData> NoobDawn::GetCaptures()
{
  SCOPED_LOCK(m_CaptureLock);
  return m_Captures;
}

void NoobDawn::MarkCaptureRetrieved(uint32_t idx)
{
  SCOPED_LOCK(m_CaptureLock);
  if(idx < m_Captures.size())
  {
    m_Captures[idx].retrieved = true;
  }
}

void NoobDawn::AddDeviceFrameCapturer(void *dev, IFrameCapturer *cap)
{
  if(IsReplayApp())
    return;

  if(dev == NULL || cap == NULL)
  {
    NBDERR("Invalid FrameCapturer %#p for device: %#p", cap, dev);
    return;
  }

  NBDLOG("Adding %s device frame capturer for %#p", ToStr(cap->GetFrameCaptureDriver()).c_str(), dev);

  SCOPED_LOCK(m_CapturerListLock);
  m_DeviceFrameCapturers[dev] = cap;
}

void NoobDawn::RemoveDeviceFrameCapturer(void *dev)
{
  if(IsReplayApp())
    return;

  if(dev == NULL)
  {
    NBDERR("Invalid device pointer: %#p", dev);
    return;
  }

  NBDLOG("Removing device frame capturer for %#p", dev);

  SCOPED_LOCK(m_CapturerListLock);
  m_DeviceFrameCapturers.erase(dev);
}

void NoobDawn::AddFrameCapturer(DeviceOwnedWindow devWnd, IFrameCapturer *cap)
{
  if(IsReplayApp())
    return;

  if(devWnd.device == NULL || devWnd.windowHandle == NULL || cap == NULL)
  {
    NBDERR("Invalid FrameCapturer %#p for combination: %#p / %#p", cap, devWnd.device,
           devWnd.windowHandle);
    return;
  }

  NBDLOG("Adding %s frame capturer for %#p / %#p", ToStr(cap->GetFrameCaptureDriver()).c_str(),
         devWnd.device, devWnd.windowHandle);

  SCOPED_LOCK(m_CapturerListLock);

  auto it = m_WindowFrameCapturers.find(devWnd);
  if(it != m_WindowFrameCapturers.end())
  {
    if(it->second.FrameCapturer != cap)
      NBDERR("New different FrameCapturer being registered for known device/window pair!");

    it->second.RefCount++;
  }
  else
  {
    m_WindowFrameCapturers[devWnd].FrameCapturer = cap;
  }

  // the first one we see becomes the default
  if(m_ActiveWindow == DeviceOwnedWindow())
    m_ActiveWindow = devWnd;
}

void NoobDawn::RemoveFrameCapturer(DeviceOwnedWindow devWnd)
{
  if(IsReplayApp())
    return;

  NBDLOG("Removing frame capturer for %#p / %#p", devWnd.device, devWnd.windowHandle);

  SCOPED_LOCK(m_CapturerListLock);

  auto it = m_WindowFrameCapturers.find(devWnd);
  if(it != m_WindowFrameCapturers.end())
  {
    it->second.RefCount--;

    if(it->second.RefCount <= 0)
    {
      NBDLOG("Removed last refcount");

      if(m_ActiveWindow == devWnd)
      {
        NBDLOG("Removed active window");

        if(m_WindowFrameCapturers.size() == 1)
        {
          m_ActiveWindow = DeviceOwnedWindow();
        }
        else
        {
          auto newactive = m_WindowFrameCapturers.begin();
          // active window could be the first in our list, move
          // to second (we know from above there are at least 2)
          if(m_ActiveWindow == newactive->first)
            newactive++;
          m_ActiveWindow = newactive->first;
        }
      }

      m_WindowFrameCapturers.erase(it);
    }
  }
  else
  {
    NBDERR("Removing FrameCapturer for unknown window!");
  }
}

bool NoobDawn::HasActiveFrameCapturer(NBDDriver driver)
{
  SCOPED_LOCK(m_CapturerListLock);

  for(auto cap = m_WindowFrameCapturers.begin(); cap != m_WindowFrameCapturers.end(); cap++)
    if(cap->second.FrameCapturer->GetFrameCaptureDriver() == driver)
      return true;

  for(auto cap = m_DeviceFrameCapturers.begin(); cap != m_DeviceFrameCapturers.end(); cap++)
    if(cap->second->GetFrameCaptureDriver() == driver)
      return true;

  return false;
}

bool NoobDawn::GetTrackedFileData(const nbdstr &nickname, bytebuf &data) const
{
  SCOPED_READLOCK(m_TrackedFilesLock);
  for(const TrackedFile &f : m_TrackedFiles)
  {
    if(f.nickname == nickname)
    {
      if(f.hasData)
        data = f.data;
      return f.hasData;
    }
  }
  return false;
}

bool NoobDawn::DoesTrackedFileExist(const nbdstr &nickname) const
{
  SCOPED_READLOCK(m_TrackedFilesLock);
  for(const TrackedFile &f : m_TrackedFiles)
  {
    if(f.nickname == nickname)
      return true;
  }
  return false;
}

// return false if the nickname already exists
bool NoobDawn::AddTrackedFileReference(const nbdstr &nickname, const nbdstr &filepath)
{
  if(DoesTrackedFileExist(nickname))
    return false;
  SCOPED_WRITELOCK(m_TrackedFilesLock);
  m_TrackedFiles.emplace_back(nickname, filepath);
  return true;
}

void NoobDawn::ClearTrackedFiles()
{
  SCOPED_WRITELOCK(m_TrackedFilesLock);
  m_TrackedFiles.clear();
}

bool NoobDawn::HasTrackedFileData() const
{
  SCOPED_READLOCK(m_TrackedFilesLock);
  return !m_TrackedFiles.empty();
}

nbdarray<nbdstr> NoobDawn::GetTrackedFileNicknames() const
{
  nbdarray<nbdstr> nickNames;
  {
    SCOPED_READLOCK(m_TrackedFilesLock);
    for(const TrackedFile &f : m_TrackedFiles)
      nickNames.push_back(f.nickname);
  }
  return nickNames;
}

RDResult NoobDawn::ReadExternalFiles(NBDFile *nbd)
{
  int32_t idx = nbd->SectionIndex(SectionType::EmbeddedExternalFiles);
  if(idx < 0)
    RETURN_WARNING_RESULT(ResultCode::DataNotAvailable, "No EmbeddedExternalFiles section");

  // int32_t countFileEntries;
  int32_t countFileEntries = 0;
  bytebuf sectionData;
  {
    StreamReader *reader = nbd->ReadSection(idx);
    sectionData.resize((size_t)reader->GetSize());
    bool success = reader->Read(sectionData.data(), reader->GetSize());
    delete reader;
    if(!success)
      sectionData.clear();
  }
  const uint32_t bufferSize = (uint32_t)sectionData.size();
  uint32_t readSize = sizeof(countFileEntries);
  uint32_t byteIndex = 0;
  if(byteIndex + readSize > bufferSize)
    RETURN_WARNING_RESULT(ResultCode::FileCorrupted,
                          "EmbeddedExternalFiles section does not have valid data");

  const byte *bufferPtr = sectionData.data();
  memcpy(&countFileEntries, bufferPtr, readSize);
  byteIndex += readSize;
  bufferPtr += readSize;

  nbdarray<TrackedFile> externalFiles;
  externalFiles.resize(countFileEntries);
  // FileEntry fileEntries[];
  for(int32_t i = 0; i < countFileEntries; ++i)
  {
    TrackedFile &externalFile = externalFiles[i];
    externalFile.filepath.clear();

    // FileEntry:
    // uint32_t nameSize;
    uint32_t nameSize = 0;
    readSize = sizeof(nameSize);
    if(byteIndex + readSize > bufferSize)
      RETURN_WARNING_RESULT(ResultCode::FileCorrupted,
                            "EmbeddedExternalFiles section does not have valid data");
    memcpy(&nameSize, bufferPtr, readSize);
    byteIndex += readSize;
    bufferPtr += readSize;

    // char name[];
    readSize = nameSize;
    if(byteIndex + readSize > bufferSize)
      RETURN_WARNING_RESULT(ResultCode::FileCorrupted,
                            "EmbeddedExternalFiles section does not have valid data");
    externalFile.nickname.assign((const char *)bufferPtr, readSize);
    byteIndex += readSize;
    bufferPtr += readSize;

    // uint32_t dataSize;
    uint32_t dataSize = 0;
    readSize = sizeof(dataSize);
    if(byteIndex + readSize > bufferSize)
      RETURN_WARNING_RESULT(ResultCode::FileCorrupted,
                            "EmbeddedExternalFiles section does not have valid data");
    memcpy(&dataSize, bufferPtr, readSize);
    byteIndex += readSize;
    bufferPtr += readSize;

    // byte data[];
    readSize = dataSize;
    if(byteIndex + readSize > bufferSize)
      RETURN_WARNING_RESULT(ResultCode::FileCorrupted,
                            "EmbeddedExternalFiles section does not have valid data");
    externalFile.data.assign(bufferPtr, readSize);
    byteIndex += readSize;
    bufferPtr += readSize;
  }
  if(byteIndex != bufferSize)
    RETURN_WARNING_RESULT(ResultCode::FileCorrupted,
                          "EmbeddedExternalFiles section does not have valid data");

  SCOPED_WRITELOCK(m_TrackedFilesLock);
  for(const TrackedFile &f : externalFiles)
  {
    for(TrackedFile &file : m_TrackedFiles)
    {
      if(file.nickname == f.nickname)
      {
        file.data = f.data;
        file.filepath.clear();
        file.hasData = true;
        break;
      }
    }
    m_TrackedFiles.emplace_back(f.nickname, f.data);
  }

  return RDResult();
}

RDResult NoobDawn::WriteExternalFiles(NBDFile *nbd, const nbdarray<TrackedFile> &trackedFiles)
{
  if(!nbd)
    RETURN_WARNING_RESULT(ResultCode::FileCorrupted,
                          "Data missing for creation of file, set metadata first.");

  RDResult nbdRes = nbd->Error();
  if(nbdRes != ResultCode::Succeeded)
    return nbdRes;

  int32_t countFileEntries = trackedFiles.count();

  bytebuf sectionData;

  // int32_t countFileEntries;
  sectionData.append((byte *)&countFileEntries, sizeof(countFileEntries));
  // FileEntry fileEntries[];
  for(const NoobDawn::TrackedFile &f : trackedFiles)
  {
    // FileEntry:
    // uint32_t nameSize;
    // char name[];
    const nbdstr &name = f.nickname;
    const uint32_t nameSize = (uint32_t)name.size();
    sectionData.append((byte *)&nameSize, sizeof(nameSize));
    sectionData.append((byte *)name.data(), nameSize);

    // uint32_t dataSize;
    // byte data[];
    uint32_t dataSize = 0;
    const byte *data = NULL;
    bytebuf fileData;
    if(!f.filepath.empty())
    {
      if(FileIO::ReadAll(f.filepath, fileData))
      {
        dataSize = (uint32_t)fileData.size();
        data = fileData.data();
      }
      else
      {
        NBDWARN("Data missing for externally referenced file %s", f.filepath.c_str());
      }
    }
    else
    {
      dataSize = (uint32_t)f.data.size();
      data = f.data.data();
    }
    sectionData.append((byte *)&dataSize, sizeof(dataSize));
    sectionData.append(data, dataSize);
  }

  SectionProperties props;
  props.type = SectionType::EmbeddedExternalFiles;
  props.flags = SectionFlags::ZstdCompressed;
  props.version = 1;

  StreamWriter *writer = nbd->WriteSection(props);
  nbdRes = nbd->Error();
  if(!writer || nbdRes != ResultCode::Succeeded)
    return nbdRes;

  writer->Write(sectionData.data(), sectionData.size());
  writer->Finish();

  delete writer;

  return RDResult();
}

RDResult NoobDawn::EmbedExternalFiles(NBDFile *nbd)
{
  if(!nbd)
    RETURN_WARNING_RESULT(ResultCode::FileCorrupted,
                          "Data missing for creation of file, set metadata first.");

  RDResult nbdRes = nbd->Error();
  if(nbdRes != ResultCode::Succeeded)
    return nbdRes;

  if(nbd->SectionIndex(SectionType::EmbeddedExternalFiles) >= 0)
    NBDWARN("Capture already has embedded external files - replacing existing section.");

  SCOPED_WRITELOCK(m_TrackedFilesLock);
  const nbdarray<NoobDawn::TrackedFile> &trackedFiles = m_TrackedFiles;
  if(trackedFiles.empty())
    NBDWARN("No external files to embed.");

  NBDLOG("Embedding %d external files", trackedFiles.count());
  return NoobDawn::Inst().WriteExternalFiles(nbd, trackedFiles);
}

RDResult NoobDawn::RemoveExternalFiles(NBDFile *nbd)
{
  if(!nbd)
    RETURN_WARNING_RESULT(ResultCode::FileCorrupted,
                          "Data missing for creation of file, set metadata first.");

  RDResult nbdRes = nbd->Error();
  if(nbdRes != ResultCode::Succeeded)
    return nbdRes;

  RDResult result;
  if(nbd->SectionIndex(SectionType::EmbeddedExternalFiles) < 0)
    RETURN_WARNING_RESULT(ResultCode::DataNotAvailable,
                          "Capture does not have any embedded external files.");

  NBDLOG("Removing embedded external files (setting count to 0)");
  nbdarray<NoobDawn::TrackedFile> trackedFiles;
  return NoobDawn::Inst().WriteExternalFiles(nbd, trackedFiles);
}

bool NoobDawn::HasEmbeddedFiles(NBDFile *nbd) const
{
  if(!nbd)
    return false;

  RDResult nbdRes = nbd->Error();
  if(nbdRes != ResultCode::Succeeded)
    return false;

  int32_t idx = nbd->SectionIndex(SectionType::EmbeddedExternalFiles);
  if(idx < 0)
    return false;

  int32_t countFileEntries = 0;
  bytebuf sectionData;
  {
    StreamReader *reader = nbd->ReadSection(idx);
    sectionData.resize((size_t)reader->GetSize());
    bool success = reader->Read(sectionData.data(), reader->GetSize());
    delete reader;
    if(!success)
      sectionData.clear();
  }
  if(sectionData.size() < sizeof(countFileEntries))
    return false;

  memcpy(&countFileEntries, sectionData.data(), sizeof(countFileEntries));
  return countFileEntries > 0;
}

#if ENABLED(ENABLE_UNIT_TESTS)

#undef None
#undef Always

#include "catch/catch.hpp"

TEST_CASE("Check ResourceId tostr", "[tostr]")
{
  union
  {
    ResourceId *id;
    uint64_t *num;
  } u;

  uint64_t data = 0;
  u.num = &data;

  *u.num = 0;
  CHECK(ToStr(*u.id) == "ResourceId::0");

  *u.num = 1;
  CHECK(ToStr(*u.id) == "ResourceId::1");

  *u.num = 7;
  CHECK(ToStr(*u.id) == "ResourceId::7");

  *u.num = 17;
  CHECK(ToStr(*u.id) == "ResourceId::17");

  *u.num = 32;
  CHECK(ToStr(*u.id) == "ResourceId::32");

  *u.num = 913;
  CHECK(ToStr(*u.id) == "ResourceId::913");

  *u.num = 454;
  CHECK(ToStr(*u.id) == "ResourceId::454");

  *u.num = 123456;
  CHECK(ToStr(*u.id) == "ResourceId::123456");

  *u.num = 1234567;
  CHECK(ToStr(*u.id) == "ResourceId::1234567");

  *u.num = 0x1234567812345678ULL;
  CHECK(ToStr(*u.id) == "ResourceId::1311768465173141112");
}

TEST_CASE("Check ResamplePixels", "[core][resamplepixels]")
{
  NoobDawn::FramePixels sourcePixels;
  uint32_t height = 4;
  uint32_t width = 4;
  uint32_t bytesPerComponent = 1;
  uint32_t countComponents = 3;
  uint32_t stride = bytesPerComponent * countComponents;
  uint32_t pitch = width * stride;
  sourcePixels.data = new byte[pitch * height];
  sourcePixels.len = 0;
  sourcePixels.width = width;
  sourcePixels.pitch = width * stride;
  sourcePixels.height = height;
  sourcePixels.stride = stride;
  sourcePixels.bpc = bytesPerComponent;
  sourcePixels.buf1010102 = false;
  sourcePixels.buf565 = false;
  sourcePixels.buf5551 = false;
  sourcePixels.bgra = false;
  sourcePixels.pitch_requirement = width;
  sourcePixels.max_width = width;

  byte *source = (byte *)sourcePixels.data;
  for(uint32_t y = 0; y < height; ++y)
  {
    for(uint32_t x = 0; x < width; ++x)
    {
      byte *src = &source[stride * x + pitch * y];
      src[0] = (byte)(y + x);
      src[1] = (byte)(y + x * 2);
      src[2] = (byte)(y + x * 3);
    }
  }

  NBDThumb thumbOutYNotFlipped;
  sourcePixels.is_y_flipped = false;
  NoobDawn::Inst().ResamplePixels(sourcePixels, thumbOutYNotFlipped);
  CHECK(thumbOutYNotFlipped.width == width);
  CHECK(thumbOutYNotFlipped.height == height);

  byte *dest = (byte *)thumbOutYNotFlipped.pixels.data();
  for(uint32_t y = 0; y < height; ++y)
  {
    for(uint32_t x = 0; x < width; ++x)
    {
      byte *src = &source[stride * x + pitch * y];
      byte *dst = &dest[stride * x + pitch * (height - y - 1)];
      CHECK((uint32_t)src[0] == (uint32_t)dst[0]);
      CHECK((uint32_t)src[1] == (uint32_t)dst[1]);
      CHECK((uint32_t)src[2] == (uint32_t)dst[2]);
    }
  }

  NBDThumb thumbOutYFlipped;
  sourcePixels.is_y_flipped = true;
  NoobDawn::Inst().ResamplePixels(sourcePixels, thumbOutYFlipped);
  CHECK(thumbOutYFlipped.width == width);
  CHECK(thumbOutYFlipped.height == height);
  dest = (byte *)thumbOutYFlipped.pixels.data();
  for(uint32_t y = 0; y < height; ++y)
  {
    for(uint32_t x = 0; x < width; ++x)
    {
      byte *src = &source[stride * x + pitch * y];
      byte *dst = &dest[stride * x + pitch * y];
      CHECK((uint32_t)src[0] == (uint32_t)dst[0]);
      CHECK((uint32_t)src[1] == (uint32_t)dst[1]);
      CHECK((uint32_t)src[2] == (uint32_t)dst[2]);
    }
  }

  NBDThumb thumbOutBGRA;
  sourcePixels.bgra = true;
  NoobDawn::Inst().ResamplePixels(sourcePixels, thumbOutBGRA);
  CHECK(thumbOutBGRA.width == width);
  CHECK(thumbOutBGRA.height == height);
  dest = (byte *)thumbOutBGRA.pixels.data();
  for(uint32_t y = 0; y < height; ++y)
  {
    for(uint32_t x = 0; x < width; ++x)
    {
      byte *src = &source[stride * x + pitch * y];
      byte *dst = &dest[stride * x + pitch * y];
      CHECK((uint32_t)src[0] == (uint32_t)dst[2]);
      CHECK((uint32_t)src[1] == (uint32_t)dst[1]);
      CHECK((uint32_t)src[2] == (uint32_t)dst[0]);
    }
  }

  NBDThumb thumbOutDownsample;
  sourcePixels.bgra = false;
  sourcePixels.max_width = 2;
  sourcePixels.pitch_requirement = 2;
  NoobDawn::Inst().ResamplePixels(sourcePixels, thumbOutDownsample);
  CHECK(thumbOutDownsample.width == 2);
  CHECK(thumbOutDownsample.height == 2);
  dest = (byte *)thumbOutDownsample.pixels.data();
  for(uint32_t y = 0; y < 2; ++y)
  {
    for(uint32_t x = 0; x < 2; ++x)
    {
      byte *src = &source[stride * x * 2 + pitch * y * 2];
      byte *dst = &dest[stride * x + pitch / 2 * y];
      CHECK((uint32_t)src[0] == (uint32_t)dst[0]);
      CHECK((uint32_t)src[1] == (uint32_t)dst[1]);
      CHECK((uint32_t)src[2] == (uint32_t)dst[2]);
    }
  }
}

#endif
