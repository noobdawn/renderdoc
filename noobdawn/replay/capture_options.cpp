/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
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

#include "api/replay/capture_options.h"
#include <float.h>
#include "api/app/noobdawn_app.h"
#include "common/common.h"
#include "core/core.h"

int NOOBDAWN_CC SetCaptureOptionU32(NOOBDAWN_CaptureOption opt, uint32_t val)
{
  CaptureOptions opts = NoobDawn::Inst().GetCaptureOptions();

  switch(opt)
  {
    case eNOOBDAWN_Option_AllowVSync: opts.allowVSync = (val != 0); break;
    case eNOOBDAWN_Option_AllowFullscreen: opts.allowFullscreen = (val != 0); break;
    case eNOOBDAWN_Option_APIValidation: opts.apiValidation = (val != 0); break;
    case eNOOBDAWN_Option_CaptureCallstacks: opts.captureCallstacks = (val != 0); break;
    case eNOOBDAWN_Option_CaptureCallstacksOnlyDraws:
      opts.captureCallstacksOnlyActions = (val != 0);
      break;
    case eNOOBDAWN_Option_DelayForDebugger: opts.delayForDebugger = val; break;
    case eNOOBDAWN_Option_VerifyBufferAccess: opts.verifyBufferAccess = (val != 0); break;
    case eNOOBDAWN_Option_HookIntoChildren: opts.hookIntoChildren = (val != 0); break;
    case eNOOBDAWN_Option_BreakACE: opts.breakACE = (val != 0); break;
    case eNOOBDAWN_Option_ExtendedHookScope: opts.extendedHookScope = (val != 0); break;
    case eNOOBDAWN_Option_EnableBlacklist: opts.enableBlacklist = (val != 0); break;
    case eNOOBDAWN_Option_EnableWhitelist: opts.enableWhitelist = (val != 0); break;
    case eNOOBDAWN_Option_RefAllResources: opts.refAllResources = (val != 0); break;
    case eNOOBDAWN_Option_SaveAllInitials:
      // option is deprecated
      break;
    case eNOOBDAWN_Option_CaptureAllCmdLists: opts.captureAllCmdLists = (val != 0); break;
    case eNOOBDAWN_Option_DebugOutputMute: opts.debugOutputMute = (val != 0); break;
    case eNOOBDAWN_Option_AllowUnsupportedVendorExtensions:
      if(val == 0x10DE)
        NoobDawn::Inst().EnableVendorExtensions(VendorExtensions::NvAPI);
      else
        NBDWARN("AllowUnsupportedVendorExtensions unexpected parameter %x", val);
      break;
    case eNOOBDAWN_Option_SoftMemoryLimit: opts.softMemoryLimit = val; break;
    default: NBDLOG("Unrecognised capture option '%d'", opt); return 0;
  }

  NoobDawn::Inst().SetCaptureOptions(opts);
  return 1;
}

int NOOBDAWN_CC SetCaptureOptionF32(NOOBDAWN_CaptureOption opt, float val)
{
  CaptureOptions opts = NoobDawn::Inst().GetCaptureOptions();

  switch(opt)
  {
    case eNOOBDAWN_Option_AllowVSync: opts.allowVSync = (val != 0.0f); break;
    case eNOOBDAWN_Option_AllowFullscreen: opts.allowFullscreen = (val != 0.0f); break;
    case eNOOBDAWN_Option_APIValidation: opts.apiValidation = (val != 0.0f); break;
    case eNOOBDAWN_Option_CaptureCallstacks: opts.captureCallstacks = (val != 0.0f); break;
    case eNOOBDAWN_Option_CaptureCallstacksOnlyDraws:
      opts.captureCallstacksOnlyActions = (val != 0.0f);
      break;
    case eNOOBDAWN_Option_DelayForDebugger: opts.delayForDebugger = (uint32_t)val; break;
    case eNOOBDAWN_Option_VerifyBufferAccess: opts.verifyBufferAccess = (val != 0.0f); break;
    case eNOOBDAWN_Option_HookIntoChildren: opts.hookIntoChildren = (val != 0.0f); break;
    case eNOOBDAWN_Option_BreakACE: opts.breakACE = (val != 0.0f); break;
    case eNOOBDAWN_Option_ExtendedHookScope: opts.extendedHookScope = (val != 0.0f); break;
    case eNOOBDAWN_Option_EnableBlacklist: opts.enableBlacklist = (val != 0.0f); break;
    case eNOOBDAWN_Option_EnableWhitelist: opts.enableWhitelist = (val != 0.0f); break;
    case eNOOBDAWN_Option_RefAllResources: opts.refAllResources = (val != 0.0f); break;
    case eNOOBDAWN_Option_SaveAllInitials:
      // option is deprecated
      break;
    case eNOOBDAWN_Option_CaptureAllCmdLists: opts.captureAllCmdLists = (val != 0.0f); break;
    case eNOOBDAWN_Option_DebugOutputMute: opts.debugOutputMute = (val != 0.0f); break;
    case eNOOBDAWN_Option_AllowUnsupportedVendorExtensions:
      NBDWARN("AllowUnsupportedVendorExtensions unexpected parameter %f", val);
      break;
    case eNOOBDAWN_Option_SoftMemoryLimit: opts.softMemoryLimit = (uint32_t)val; break;
    default: NBDLOG("Unrecognised capture option '%d'", opt); return 0;
  }

  NoobDawn::Inst().SetCaptureOptions(opts);
  return 1;
}

uint32_t NOOBDAWN_CC GetCaptureOptionU32(NOOBDAWN_CaptureOption opt)
{
  switch(opt)
  {
    case eNOOBDAWN_Option_AllowVSync:
      return (NoobDawn::Inst().GetCaptureOptions().allowVSync ? 1 : 0);
    case eNOOBDAWN_Option_AllowFullscreen:
      return (NoobDawn::Inst().GetCaptureOptions().allowFullscreen ? 1 : 0);
    case eNOOBDAWN_Option_APIValidation:
      return (NoobDawn::Inst().GetCaptureOptions().apiValidation ? 1 : 0);
    case eNOOBDAWN_Option_CaptureCallstacks:
      return (NoobDawn::Inst().GetCaptureOptions().captureCallstacks ? 1 : 0);
    case eNOOBDAWN_Option_CaptureCallstacksOnlyDraws:
      return (NoobDawn::Inst().GetCaptureOptions().captureCallstacksOnlyActions ? 1 : 0);
    case eNOOBDAWN_Option_DelayForDebugger:
      return (NoobDawn::Inst().GetCaptureOptions().delayForDebugger);
    case eNOOBDAWN_Option_VerifyBufferAccess:
      return (NoobDawn::Inst().GetCaptureOptions().verifyBufferAccess ? 1 : 0);
    case eNOOBDAWN_Option_HookIntoChildren:
      return (NoobDawn::Inst().GetCaptureOptions().hookIntoChildren ? 1 : 0);
    case eNOOBDAWN_Option_BreakACE:
      return (NoobDawn::Inst().GetCaptureOptions().breakACE ? 1 : 0);
    case eNOOBDAWN_Option_ExtendedHookScope:
      return (NoobDawn::Inst().GetCaptureOptions().extendedHookScope ? 1 : 0);
    case eNOOBDAWN_Option_EnableBlacklist:
      return (NoobDawn::Inst().GetCaptureOptions().enableBlacklist ? 1 : 0);
    case eNOOBDAWN_Option_EnableWhitelist:
      return (NoobDawn::Inst().GetCaptureOptions().enableWhitelist ? 1 : 0);
    case eNOOBDAWN_Option_RefAllResources:
      return (NoobDawn::Inst().GetCaptureOptions().refAllResources ? 1 : 0);
    case eNOOBDAWN_Option_SaveAllInitials:
      // option is deprecated - always enabled
      return 1;
    case eNOOBDAWN_Option_CaptureAllCmdLists:
      return (NoobDawn::Inst().GetCaptureOptions().captureAllCmdLists ? 1 : 0);
    case eNOOBDAWN_Option_DebugOutputMute:
      return (NoobDawn::Inst().GetCaptureOptions().debugOutputMute ? 1 : 0);
    case eNOOBDAWN_Option_AllowUnsupportedVendorExtensions: return 0;
    case eNOOBDAWN_Option_SoftMemoryLimit:
      return (NoobDawn::Inst().GetCaptureOptions().softMemoryLimit);
    default: break;
  }

  NBDLOG("Unrecognised capture option '%d'", opt);
  return 0xffffffff;
}

float NOOBDAWN_CC GetCaptureOptionF32(NOOBDAWN_CaptureOption opt)
{
  switch(opt)
  {
    case eNOOBDAWN_Option_AllowVSync:
      return (NoobDawn::Inst().GetCaptureOptions().allowVSync ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_AllowFullscreen:
      return (NoobDawn::Inst().GetCaptureOptions().allowFullscreen ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_APIValidation:
      return (NoobDawn::Inst().GetCaptureOptions().apiValidation ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_CaptureCallstacks:
      return (NoobDawn::Inst().GetCaptureOptions().captureCallstacks ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_CaptureCallstacksOnlyDraws:
      return (NoobDawn::Inst().GetCaptureOptions().captureCallstacksOnlyActions ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_DelayForDebugger:
      return (NoobDawn::Inst().GetCaptureOptions().delayForDebugger * 1.0f);
    case eNOOBDAWN_Option_VerifyBufferAccess:
      return (NoobDawn::Inst().GetCaptureOptions().verifyBufferAccess ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_HookIntoChildren:
      return (NoobDawn::Inst().GetCaptureOptions().hookIntoChildren ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_BreakACE:
      return (NoobDawn::Inst().GetCaptureOptions().breakACE ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_ExtendedHookScope:
      return (NoobDawn::Inst().GetCaptureOptions().extendedHookScope ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_EnableBlacklist:
      return (NoobDawn::Inst().GetCaptureOptions().enableBlacklist ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_EnableWhitelist:
      return (NoobDawn::Inst().GetCaptureOptions().enableWhitelist ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_RefAllResources:
      return (NoobDawn::Inst().GetCaptureOptions().refAllResources ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_SaveAllInitials:
      // option is deprecated - always enabled
      return 1.0f;
    case eNOOBDAWN_Option_CaptureAllCmdLists:
      return (NoobDawn::Inst().GetCaptureOptions().captureAllCmdLists ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_DebugOutputMute:
      return (NoobDawn::Inst().GetCaptureOptions().debugOutputMute ? 1.0f : 0.0f);
    case eNOOBDAWN_Option_AllowUnsupportedVendorExtensions: return 0.0f;
    case eNOOBDAWN_Option_SoftMemoryLimit:
      return (NoobDawn::Inst().GetCaptureOptions().softMemoryLimit * 1.0f);
    default: break;
  }

  NBDLOG("Unrecognised capture option '%d'", opt);
  return -FLT_MAX;
}

CaptureOptions::CaptureOptions()
{
  // since we're reading from all bytes even padding etc, memset to 0
  NBDEraseEl(*this);
  allowVSync = true;
  allowFullscreen = true;
  apiValidation = false;
  captureCallstacks = false;
  captureCallstacksOnlyActions = false;
  delayForDebugger = 0;
  verifyBufferAccess = false;
  hookIntoChildren = false;
  breakACE = false;
  extendedHookScope = false;
  enableBlacklist = false;
  enableWhitelist = false;
  refAllResources = false;
  captureAllCmdLists = false;
  debugOutputMute = true;
  softMemoryLimit = 0;
}

#if ENABLED(ENABLE_UNIT_TESTS)

#undef None
#undef Always

#include "catch/catch.hpp"

TEST_CASE("Check CaptureOptions de/serialise to string", "[serialise]")
{
  CaptureOptions opts;

  bool *boolOpts[] = {
      &opts.allowVSync,
      &opts.allowFullscreen,
      &opts.apiValidation,
      &opts.captureCallstacks,
      &opts.captureCallstacksOnlyActions,
      &opts.verifyBufferAccess,
      &opts.hookIntoChildren,
      &opts.breakACE,
      &opts.extendedHookScope,
      &opts.enableBlacklist,
      &opts.enableWhitelist,
      &opts.refAllResources,
      &opts.captureAllCmdLists,
      &opts.debugOutputMute,
  };

  for(uint32_t delay = 0; delay < 1000; delay++)
  {
    for(uint32_t variant = 0; variant < (1 << ARRAY_COUNT(boolOpts)); variant++)
    {
      opts.delayForDebugger = delay;
      for(size_t o = 0; o < ARRAY_COUNT(boolOpts); o++)
      {
        *boolOpts[o] = (variant & (1 << o)) != 0;
      }

      nbdstr s = opts.EncodeAsString();
      CaptureOptions decoded;
      decoded.DecodeFromString(s);

      CHECK(memcmp(&opts, &decoded, sizeof(decoded)) == 0);
    }
  }

  // check that nothing explodes here
  CaptureOptions a;
  a.DecodeFromString("");
}

#endif
