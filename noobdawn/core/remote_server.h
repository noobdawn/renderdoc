/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2026 Baldur Karlsson
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

#include "api/replay/noobdawn_replay.h"
#include "os/os_specific.h"

namespace Network
{
class Socket;
};

enum class NBDDriver : uint32_t;

class WriteSerialiser;
class ReadSerialiser;

struct RemoteServer : public IRemoteServer
{
public:
  RemoteServer(Network::Socket *sock, const nbdstr &deviceID);

  virtual ~RemoteServer();

  virtual void ShutdownConnection();

  virtual void ShutdownServerAndConnection();

  virtual bool Connected();
  virtual ResultDetails Ping();

  virtual nbdarray<nbdstr> LocalProxies();

  virtual nbdarray<nbdstr> RemoteSupportedReplays();

  virtual nbdstr GetHomeFolder();

  virtual nbdarray<PathEntry> ListFolder(const nbdstr &path);

  virtual ExecuteResult ExecuteAndInject(const nbdstr &app, const nbdstr &workingDir,
                                         const nbdstr &cmdline,
                                         const nbdarray<EnvironmentModification> &env,
                                         const CaptureOptions &opts, const nbdstr &blacklist);

  virtual void CopyCaptureFromRemote(const nbdstr &remotepath, const nbdstr &localpath,
                                     NOOBDAWN_ProgressCallback progress);

  virtual nbdstr CopyCaptureToRemote(const nbdstr &filename, NOOBDAWN_ProgressCallback progress);

  virtual void TakeOwnershipCapture(const nbdstr &filename);

  virtual nbdpair<ResultDetails, IReplayController *> OpenCapture(uint32_t proxyid,
                                                                  const nbdstr &filename,
                                                                  const ReplayOptions &opts,
                                                                  NOOBDAWN_ProgressCallback progress);

  virtual void CloseCapture(IReplayController *rend);

  virtual nbdstr DriverName();

  virtual nbdarray<GPUDevice> GetAvailableGPUs();

  virtual int32_t GetSectionCount();

  virtual int32_t FindSectionByName(const nbdstr &name);

  virtual int32_t FindSectionByType(SectionType sectionType);

  virtual SectionProperties GetSectionProperties(int32_t index);

  virtual bytebuf GetSectionContents(int32_t index);

  virtual ResultDetails WriteSection(const SectionProperties &props, const bytebuf &contents);

  virtual bool HasCallstacks();

  virtual ResultDetails InitResolver(bool interactive, NOOBDAWN_ProgressCallback progress);

  virtual nbdarray<nbdstr> GetResolve(const nbdarray<uint64_t> &callstack);

  virtual ResultDetails EmbedDependenciesIntoCapture();
  virtual ResultDetails RemoveDependenciesFromCapture();
  virtual bool HasEmbeddedDependencies();
  virtual bool HasPendingDependencies();
  virtual nbdarray<nbdstr> GetPendingDependenciesNicknames();

protected:
  Network::Socket *m_Socket;
  WriteSerialiser *writer;
  ReadSerialiser *reader;
  FileIO::LogFileHandle *debugLog;
  nbdstr m_deviceID;

  nbdarray<nbdpair<NBDDriver, nbdstr>> m_Proxies;
};
