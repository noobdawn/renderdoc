/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2018-2026 Baldur Karlsson
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

#pragma once

#include "api/replay/stringise.h"
#include "android.h"

// internal functions, shouldn't be used outside the android implementation - anything public goes
// in android.h

namespace Android
{
Process::ProcessResult execScript(const nbdstr &script, const nbdstr &args,
                                  const nbdstr &workDir = ".", bool silent = false);
Process::ProcessResult execCommand(const nbdstr &exe, const nbdstr &args,
                                   const nbdstr &workDir = ".", bool silent = false);

enum class ToolDir
{
  None,
  Java,
  BuildTools,
  BuildToolsLib,
  PlatformTools,
};
nbdstr getToolPath(ToolDir subdir, const nbdstr &toolname, bool checkExist);
bool toolExists(const nbdstr &path);

bool IsDebuggable(const nbdstr &deviceID, const nbdstr &packageName);
bool HasRootAccess(const nbdstr &deviceID);
nbdstr GetFirstMatchingLine(const nbdstr &haystack, const nbdstr &needle);

bool IsSupported(nbdstr deviceID);
bool SupportsNativeLayers(const nbdstr &deviceID);
nbdstr DetermineInstalledABI(const nbdstr &deviceID, const nbdstr &packageName);
nbdstr GetFriendlyName(const nbdstr &deviceID);

// supported ABIs
enum class ABI
{
  unknown,
  armeabi_v7a,
  arm64_v8a,
  x86,
  x86_64,
};

ABI GetABI(const nbdstr &abiName);
nbdstr GetPlainABIName(ABI abi);
nbdarray<ABI> GetSupportedABIs(const nbdstr &deviceID);
nbdstr GetNoobDawnPackageForABI(ABI abi);
nbdstr GetPathForPackage(const nbdstr &deviceID, const nbdstr &packageName);
nbdstr GetFolderName(const nbdstr &deviceID);
};

DECLARE_REFLECTION_ENUM(Android::ABI);
