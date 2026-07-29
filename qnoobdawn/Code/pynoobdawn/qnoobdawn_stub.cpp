/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2017-2026 Baldur Karlsson
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

#include <Python.h>

#define SWIG_GENERATED
#include "Code/Interface/QRDInterface.h"

// we only support the qnoobdawn module for docs generation, so it doesn't matter that these stub
// functions aren't valid

////////////////////////////////////////////////////////////////////////////////
// QRDInterface.cpp stubs
////////////////////////////////////////////////////////////////////////////////

CaptureSettings::CaptureSettings()
{
  inject = false;
  autoStart = false;
  queuedFrameCap = 0;
  numQueuedFrames = 0;
  NOOBDAWN_GetDefaultCaptureOptions(&options);
}

nbdstr ConfigFilePath(const nbdstr &filename)
{
  return "";
}

////////////////////////////////////////////////////////////////////////////////
// PythonContext.cpp stubs
////////////////////////////////////////////////////////////////////////////////

class QWidget;

extern "C" QWidget *QWidgetFromPy(PyObject *widget)
{
  return NULL;
}

extern "C" PyObject *QWidgetToPy(QWidget *widget)
{
  Py_IncRef(Py_None);
  return Py_None;
}

////////////////////////////////////////////////////////////////////////////////
// ShaderProcessingTool.cpp stubs
////////////////////////////////////////////////////////////////////////////////

nbdstr ShaderProcessingTool::DefaultArguments() const
{
  return "";
}

nbdstr ShaderProcessingTool::IOArguments() const
{
  return "";
}

ShaderToolOutput ShaderProcessingTool::DisassembleShader(QWidget *window,
                                                         const ShaderReflection *shaderDetails,
                                                         nbdstr arguments) const
{
  return {};
}

ShaderToolOutput ShaderProcessingTool::CompileShader(QWidget *window, nbdstr source,
                                                     nbdstr entryPoint, ShaderStage stage,
                                                     nbdstr spirvVer, nbdstr arguments) const
{
  return {};
}

////////////////////////////////////////////////////////////////////////////////
// PersistantConfig.cpp stubs
////////////////////////////////////////////////////////////////////////////////

nbdstr BugReport::URL() const
{
  return "";
}

bool PersistantConfig::SetStyle()
{
  return false;
}

PersistantConfig::PersistantConfig()
{
}

PersistantConfig::~PersistantConfig()
{
}

bool PersistantConfig::Load(const nbdstr &filename)
{
  return false;
}

bool PersistantConfig::Save()
{
  return false;
}

void PersistantConfig::Close()
{
}

nbdarray<RemoteHost> PersistantConfig::GetRemoteHosts()
{
  return {};
}

RemoteHost PersistantConfig::GetRemoteHost(const nbdstr &)
{
  return RemoteHost();
}

void PersistantConfig::AddRemoteHost(RemoteHost host)
{
}

void PersistantConfig::RemoveRemoteHost(RemoteHost host)
{
}

void PersistantConfig::UpdateEnumeratedProtocolDevices()
{
}

void PersistantConfig::SetupFormatting()
{
}

void AddRecentFile(nbdarray<nbdstr> &recentList, const nbdstr &file)
{
}

void RemoveRecentFile(nbdarray<nbdstr> &recentList, const nbdstr &file)
{
}

////////////////////////////////////////////////////////////////////////////////
// RemoteHost.cpp stubs
////////////////////////////////////////////////////////////////////////////////

RemoteHost::RemoteHost()
{
}

RemoteHost::RemoteHost(const nbdstr &host)
{
}

RemoteHost::RemoteHost(const RemoteHost &o)
{
}

RemoteHost &RemoteHost::operator=(const RemoteHost &o)
{
  return *this;
}

RemoteHost::~RemoteHost()
{
}

void RemoteHost::CheckStatus()
{
}

ResultDetails RemoteHost::Connect(IRemoteServer **server)
{
  return {ResultCode::Succeeded};
}

ResultDetails RemoteHost::Launch()
{
  return {ResultCode::Succeeded};
}

bool RemoteHost::IsServerRunning() const
{
  return false;
}

bool RemoteHost::IsConnected() const
{
  return false;
}

bool RemoteHost::IsBusy() const
{
  return false;
}

bool RemoteHost::IsVersionMismatch() const
{
  return false;
}

nbdstr RemoteHost::VersionMismatchError() const
{
  return nbdstr();
}

nbdstr RemoteHost::FriendlyName() const
{
  return nbdstr();
}

void RemoteHost::SetFriendlyName(const nbdstr &name)
{
}

nbdstr RemoteHost::RunCommand() const
{
  return nbdstr();
}

void RemoteHost::SetRunCommand(const nbdstr &cmd)
{
}

nbdstr RemoteHost::LastCapturePath() const
{
  return nbdstr();
}

void RemoteHost::SetLastCapturePath(const nbdstr &path)
{
}

void RemoteHost::SetConnected(bool connected)
{
}

void RemoteHost::SetShutdown()
{
}
