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

#include "remote_server.h"
#include <utility>
#include "android/android.h"
#include "api/replay/noobdawn_replay.h"
#include "api/replay/version.h"
#include "common/threading.h"
#include "core/core.h"
#include "core/settings.h"
#include "os/os_specific.h"
#include "replay/replay_controller.h"
#include "serialise/nbdfile.h"
#include "serialise/serialiser.h"
#include "strings/string_utils.h"
#include "replay_proxy.h"

RDOC_CONFIG(uint32_t, RemoteServer_TimeoutMS, 5000,
            "Timeout in milliseconds for remote server operations.");

RDOC_CONFIG(bool, RemoteServer_DebugLogging, false,
            "Output a verbose logging file in the system's temporary folder containing the "
            "traffic to and from the remote server.");

#define MAKE_REMOTE_SERVER_VERSION(maj, min) uint32_t((maj)*1000) + (min)

static const uint32_t RemoteServerProtocolVersion =
    MAKE_REMOTE_SERVER_VERSION(NOOBDAWN_VERSION_MAJOR, NOOBDAWN_VERSION_MINOR);

enum class RemoteServerPacket
{
  // fixed packets. These are used cross-version so MUST NOT CHANGE
  Noop = 1,
  Handshake,
  VersionMismatch,
  Busy,
  VersionMismatch2,    // sent for versions 1.23 and above, including the version info

  // variable packets. These are used only after a handshake has been established with an identical
  // version so can be freely changed
  Ping,
  RemoteDriverList,
  TakeOwnershipCapture,
  CopyCaptureToRemote,
  CopyCaptureFromRemote,
  OpenLog,
  LogOpenProgress,
  LogOpened,
  HasCallstacks,
  InitResolver,
  ResolverProgress,
  GetResolve,
  CloseLog,
  HomeDir,
  ListDir,
  ExecuteAndInject,
  ShutdownServer,
  GetDriverName,
  GetSectionCount,
  FindSectionByName,
  FindSectionByType,
  GetSectionProperties,
  GetSectionContents,
  WriteSection,
  GetAvailableGPUs,
  EmbedDependenciesIntoCapture,
  RemoveDependenciesFromCapture,
  HasEmbeddedDependencies,
  HasPendingDependencies,
  GetPendingDependenciesNicknames,
  // This must be last
  Count,
};

DECLARE_REFLECTION_ENUM(RemoteServerPacket);

NBDCOMPILE_ASSERT((int)RemoteServerPacket::Count < (int)eReplayProxy_First,
                  "Remote server and Replay Proxy packets overlap");

template <>
nbdstr DoStringise(const RemoteServerPacket &el)
{
  BEGIN_ENUM_STRINGISE(RemoteServerPacket);
  {
    STRINGISE_ENUM_NAMED(RemoteServerPacket::Noop, "No-op");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::Handshake, "Handshake");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::VersionMismatch, "VersionMismatch");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::Busy, "Busy");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::VersionMismatch2, "VersionMismatch");

    STRINGISE_ENUM_NAMED(RemoteServerPacket::Ping, "Ping");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::RemoteDriverList, "RemoteDriverList");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::TakeOwnershipCapture, "TakeOwnershipCapture");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::CopyCaptureToRemote, "CopyCaptureToRemote");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::CopyCaptureFromRemote, "CopyCaptureFromRemote");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::OpenLog, "OpenLog");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::LogOpenProgress, "LogOpenProgress");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::LogOpened, "LogOpened");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::HasCallstacks, "HasCallstacks");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::InitResolver, "InitResolver");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::ResolverProgress, "ResolverProgress");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::GetResolve, "GetResolve");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::CloseLog, "CloseLog");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::HomeDir, "HomeDir");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::ListDir, "ListDir");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::ExecuteAndInject, "ExecuteAndInject");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::ShutdownServer, "ShutdownServer");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::GetDriverName, "GetDriverName");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::GetSectionCount, "GetSectionCount");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::FindSectionByName, "FindSectionByName");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::FindSectionByType, "FindSectionByType");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::GetSectionProperties, "GetSectionProperties");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::GetSectionContents, "GetSectionContents");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::WriteSection, "WriteSection");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::GetAvailableGPUs, "GetAvailableGPUs");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::EmbedDependenciesIntoCapture,
                         "EmbedDependenciesIntoCapture");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::RemoveDependenciesFromCapture,
                         "RemoveDependenciesFromCapture");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::HasEmbeddedDependencies, "HasEmbeddedDependencies");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::HasPendingDependencies, "HasPendingDependencies");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::GetPendingDependenciesNicknames,
                         "GetPendingDependenciesNicknames");
    STRINGISE_ENUM_NAMED(RemoteServerPacket::Count, "Count");
  }
  END_ENUM_STRINGISE();
}

nbdstr GetRemoteServerChunkName(uint32_t idx)
{
  if(idx <= (uint32_t)RemoteServerPacket::Count)
    return ToStr((RemoteServerPacket)idx);

  if(idx >= eReplayProxy_First)
    return ToStr((ReplayProxyPacket)idx);

  return StringFormat::Fmt("Invalid RemoteServerChunkIndex %u", idx);
}

#define WRITE_DATA_SCOPE() WriteSerialiser &ser = writer;
#define READ_DATA_SCOPE() ReadSerialiser &ser = reader;

struct ClientThread
{
  ClientThread()
      : socket(NULL), allowExecution(false), killThread(false), killServer(false), thread(0)
  {
  }

  Network::Socket *socket;

  bool allowExecution;
  bool killThread;
  bool killServer;

  Threading::ThreadHandle thread;
};

struct ActiveClient
{
  Threading::CriticalSection lock;
  ClientThread *active = NULL;
};

static bool HandleHandshakeClient(ActiveClient &activeClient, ClientThread *threadData)
{
  uint32_t ip = threadData->socket->GetRemoteIP();

  uint32_t version = 0;

  bool activeConnectionDesired = false;
  bool activeConnectionEstablished = false;

  {
    ReadSerialiser ser(new StreamReader(threadData->socket, Ownership::Nothing), Ownership::Stream);

    ser.SetStreamingMode(true);

    // this thread just handles receiving the handshake and sending a busy signal without blocking
    // the server thread
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(ser.IsErrored() || type != RemoteServerPacket::Handshake)
    {
      NBDWARN("Didn't receive proper handshake");
      return activeConnectionEstablished;
    }

    SERIALISE_ELEMENT(version);
    SERIALISE_ELEMENT(activeConnectionDesired);

    ser.EndChunk();
  }

  {
    WriteSerialiser ser(new StreamWriter(threadData->socket, Ownership::Nothing), Ownership::Stream);

    ser.SetStreamingMode(true);

    if(version != RemoteServerProtocolVersion)
    {
      NBDLOG("Connection using protocol %u, but we are running %u", version,
             RemoteServerProtocolVersion);

      // as of 1.23 we started serialising our version so the other end knows what it's talking
      // to.
      if(version >= MAKE_REMOTE_SERVER_VERSION(1, 23))
      {
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::VersionMismatch2);

        SERIALISE_ELEMENT(RemoteServerProtocolVersion);
      }
      else
      {
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::VersionMismatch);
      }
    }
    else
    {
      bool busy = false;

      {
        SCOPED_LOCK(activeClient.lock);
        busy = activeClient.active != NULL;

        // if we're not busy, and the connection wants to be active, promote it.
        if(!busy && activeConnectionDesired)
        {
          NBDLOG("Promoting connection from %u.%u.%u.%u to active.", Network::GetIPOctet(ip, 0),
                 Network::GetIPOctet(ip, 1), Network::GetIPOctet(ip, 2), Network::GetIPOctet(ip, 3));
          activeConnectionEstablished = true;
          activeClient.active = threadData;
        }
      }

      // if we were busy, return that status
      if(busy)
      {
        NBDLOG("Returning busy signal for connection from %u.%u.%u.%u.", Network::GetIPOctet(ip, 0),
               Network::GetIPOctet(ip, 1), Network::GetIPOctet(ip, 2), Network::GetIPOctet(ip, 3));

        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::Busy);
      }
      // otherwise we return a successful handshake. For active connections this begins the active
      // thread, for passive connection checks this is enough
      else
      {
        NBDLOG("Returning OK signal for connection from %u.%u.%u.%u.", Network::GetIPOctet(ip, 0),
               Network::GetIPOctet(ip, 1), Network::GetIPOctet(ip, 2), Network::GetIPOctet(ip, 3));

        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::Handshake);
      }
    }
  }

  // return whether or not an active connection was established
  return activeConnectionEstablished;
}

static void ActiveRemoteClientThread(ClientThread *threadData,
                                     NOOBDAWN_PreviewWindowCallback previewWindow)
{
  Threading::SetCurrentThreadName("ActiveRemoteClientThread");

  Network::Socket *&client = threadData->socket;

  client->SetTimeout(RemoteServer_TimeoutMS());

  uint32_t ip = client->GetRemoteIP();

  nbdarray<nbdstr> tempFiles;
  IRemoteDriver *remoteDriver = NULL;
  IReplayDriver *replayDriver = NULL;
  ReplayProxy *proxy = NULL;
  NBDFile *nbd = NULL;
  Callstack::StackResolver *resolver = NULL;

  FileIO::LogFileHandle *debugLog = NULL;

  WriteSerialiser writer(new StreamWriter(client, Ownership::Nothing), Ownership::Stream);
  ReadSerialiser reader(new StreamReader(client, Ownership::Nothing), Ownership::Stream);

  if(RemoteServer_DebugLogging())
  {
    reader.ConfigureStructuredExport(&GetRemoteServerChunkName, false, 0, 1.0);
    writer.ConfigureStructuredExport(&GetRemoteServerChunkName, false, 0, 1.0);

    nbdstr filename = FileIO::GetTempFolderFilename() + "/NoobDawn/RemoteServer_Server.log";

    NBDLOG("Logging remote server work to '%s'", filename.c_str());

    // truncate the log
    debugLog = FileIO::logfile_open(filename);
    FileIO::logfile_close(debugLog, filename);
    debugLog = FileIO::logfile_open(filename);

    reader.EnableDumping(debugLog);
    writer.EnableDumping(debugLog);
  }

  writer.SetStreamingMode(true);
  reader.SetStreamingMode(true);

  uint32_t captureNum = 0;

  while(client)
  {
    if(client && !client->Connected())
      break;

    if(threadData->killThread)
      break;

    // this will block until a packet comes in.
    RemoteServerPacket type = reader.ReadChunk<RemoteServerPacket>();

    if(reader.IsErrored() || writer.IsErrored())
      break;

    if(client == NULL)
      continue;

    if(type == RemoteServerPacket::Ping)
    {
      reader.EndChunk();

      if(proxy)
        proxy->RefreshPreviewWindow();

      // insert a dummy line into our logcat so we can keep track of our progress
      Android::TickDeviceLogcat();

      WRITE_DATA_SCOPE();
      SCOPED_SERIALISE_CHUNK(RemoteServerPacket::Ping);
    }
    else if(type == RemoteServerPacket::RemoteDriverList)
    {
      reader.EndChunk();

      std::map<NBDDriver, nbdstr> drivers = NoobDawn::Inst().GetRemoteDrivers();
      uint32_t count = (uint32_t)drivers.size();

      WRITE_DATA_SCOPE();
      SCOPED_SERIALISE_CHUNK(RemoteServerPacket::RemoteDriverList);
      SERIALISE_ELEMENT(count);

      for(auto it = drivers.begin(); it != drivers.end(); ++it)
      {
        NBDDriver driverType = it->first;
        const nbdstr &driverName = it->second;

        SERIALISE_ELEMENT(driverType);
        SERIALISE_ELEMENT(driverName);
      }
    }
    else if(type == RemoteServerPacket::HomeDir)
    {
      reader.EndChunk();

      nbdstr home = FileIO::GetHomeFolderFilename();

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::HomeDir);
        SERIALISE_ELEMENT(home);
      }
    }
    else if(type == RemoteServerPacket::ListDir)
    {
      nbdstr path;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(path);
      }

      reader.EndChunk();

      nbdarray<PathEntry> files;
      FileIO::GetFilesInDirectory(path, files);

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::ListDir);
        SERIALISE_ELEMENT(files);
      }
    }
    else if(type == RemoteServerPacket::CopyCaptureFromRemote)
    {
      nbdstr path;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(path);
      }

      reader.EndChunk();

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::CopyCaptureFromRemote);

        StreamReader fileStream(FileIO::fopen(path, FileIO::ReadBinary));
        ser.SerialiseStream(path, fileStream);
      }
    }
    else if(type == RemoteServerPacket::CopyCaptureToRemote)
    {
      nbdstr path;
      nbdstr dummy, dummy2;
      FileIO::GetDefaultFiles("remotecopy", path, dummy, dummy2);

      // remove the .nbd
      path.erase(path.size() - 4, 4);

      // append a process- and capture- specific suffix to avoid clashes
      path += StringFormat::Fmt("_remotecopy_%u_%u.nbd", Process::GetCurrentPID(), captureNum);
      captureNum++;

      NBDLOG("Copying file to local path '%s'.", path.c_str());

      FileIO::CreateParentDirectory(path);

      {
        READ_DATA_SCOPE();

        StreamWriter streamWriter(FileIO::fopen(path, FileIO::WriteBinary), Ownership::Stream);

        ser.SerialiseStream(path, streamWriter, NULL);
      }

      reader.EndChunk();

      if(reader.IsErrored())
      {
        FileIO::Delete(path);

        NBDERR("Network error receiving file");
        break;
      }

      NBDLOG("File received.");

      tempFiles.push_back(path);

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::CopyCaptureToRemote);
        SERIALISE_ELEMENT(path);
      }
    }
    else if(type == RemoteServerPacket::TakeOwnershipCapture)
    {
      nbdstr path;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(path);
      }

      reader.EndChunk();

      NBDLOG("Taking ownership of capture.");

      tempFiles.push_back(path);
    }
    else if(type == RemoteServerPacket::GetAvailableGPUs)
    {
      reader.EndChunk();

      nbdarray<GPUDevice> gpus = NoobDawn::Inst().GetAvailableGPUs();

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetAvailableGPUs);
        SERIALISE_ELEMENT(gpus);
      }
    }
    else if(type == RemoteServerPacket::ShutdownServer)
    {
      reader.EndChunk();

      NBDLOG("Requested to shut down.");

      threadData->killServer = true;
      threadData->killThread = true;

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::ShutdownServer);
      }
    }
    else if(type == RemoteServerPacket::OpenLog)
    {
      nbdstr path;
      ReplayOptions opts;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(path);
        SERIALISE_ELEMENT(opts);
      }

      reader.EndChunk();

      NBDASSERT(remoteDriver == NULL && proxy == NULL && nbd == NULL);

      nbd = new NBDFile();
      nbd->Open(path);

      RDResult result = nbd->Error();

      if(result == ResultCode::Succeeded)
      {
        if(NoobDawn::Inst().HasRemoteDriver(nbd->GetDriver()))
        {
          bool kill = false;
          float progress = 0.0f;

          NoobDawn::Inst().SetProgressCallback<LoadProgress>([&progress](float p) { progress = p; });

          Threading::ThreadHandle ticker = Threading::CreateThread([&writer, &kill, &progress]() {
            while(!kill)
            {
              {
                WRITE_DATA_SCOPE();
                SCOPED_SERIALISE_CHUNK(RemoteServerPacket::LogOpenProgress);
                SERIALISE_ELEMENT(progress);
              }
              Threading::Sleep(100);
            }
          });

          // This has to be before the driver is created for the capture
          NoobDawn::Inst().ClearTrackedFiles();
          if(nbd->SectionIndex(SectionType::EmbeddedExternalFiles) >= 0)
          {
            ResultDetails ret = NoobDawn::Inst().ReadExternalFiles(nbd);
            if(!ret.OK())
            {
              NBDERR("ReadExternalFiles failed Code:'%s' Message:'%s'", ToStr(ret.code).c_str(),
                     ret.Message().c_str());
            }
          }

          // if we have a replay driver, try to create it so we can display a local preview e.g.
          if(NoobDawn::Inst().HasReplayDriver(nbd->GetDriver()))
          {
            result = NoobDawn::Inst().CreateReplayDriver(nbd, opts, &replayDriver);
            if(replayDriver)
              remoteDriver = replayDriver;
          }
          else
          {
            result = NoobDawn::Inst().CreateRemoteDriver(nbd, opts, &remoteDriver);
          }

          if(result != ResultCode::Succeeded || remoteDriver == NULL)
          {
            NBDERR("Failed to create remote driver for driver '%s'", nbd->GetDriverName().c_str());
          }
          else
          {
            result = remoteDriver->ReadLogInitialisation(nbd, false);

            if(result != ResultCode::Succeeded)
            {
              NBDERR("Failed to initialise remote driver.");

              remoteDriver->Shutdown();
              remoteDriver = NULL;
            }
          }

          NoobDawn::Inst().SetProgressCallback<LoadProgress>(NOOBDAWN_ProgressCallback());

          kill = true;
          Threading::JoinThread(ticker);
          Threading::CloseThread(ticker);

          if(result == ResultCode::Succeeded && remoteDriver)
          {
            proxy = new ReplayProxy(reader, writer, remoteDriver, replayDriver, previewWindow);
          }
        }
        else
        {
          SET_ERROR_RESULT(result, ResultCode::APIUnsupported,
                           "File needs driver for '%s' which isn't supported!",
                           nbd->GetDriverName().c_str());
        }
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::LogOpened);
        SERIALISE_ELEMENT(result);
      }
    }
    else if(type == RemoteServerPacket::HasCallstacks)
    {
      reader.EndChunk();

      bool HasCallstacks = nbd && nbd->SectionIndex(SectionType::ResolveDatabase) >= 0;

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::HasCallstacks);
        SERIALISE_ELEMENT(HasCallstacks);
      }
    }
    else if(type == RemoteServerPacket::InitResolver)
    {
      reader.EndChunk();

      RDResult res;

      int sectionIndex = nbd ? nbd->SectionIndex(SectionType::ResolveDatabase) : -1;

      SAFE_DELETE(resolver);
      if(sectionIndex >= 0)
      {
        StreamReader *sectionReader = nbd->ReadSection(sectionIndex);

        bytebuf buf;
        buf.resize((size_t)sectionReader->GetSize());
        bool success = sectionReader->Read(buf.data(), sectionReader->GetSize());

        res = sectionReader->GetError();

        delete sectionReader;

        if(success && res == ResultCode::Succeeded)
        {
          float progress = 0.0f;

          Threading::ThreadHandle ticker = Threading::CreateThread([&writer, &resolver, &progress]() {
            while(!resolver)
            {
              {
                WRITE_DATA_SCOPE();
                SCOPED_SERIALISE_CHUNK(RemoteServerPacket::ResolverProgress);
                SERIALISE_ELEMENT(progress);
              }
              Threading::Sleep(100);
            }
          });

          resolver = Callstack::MakeResolver(false, buf.data(), buf.size(),
                                             [&progress](float p) { progress = p; });

          Threading::JoinThread(ticker);
          Threading::CloseThread(ticker);
        }
        else
        {
          res.message = "Failed to read resolve database. " + res.message;
        }
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::InitResolver);
        SERIALISE_ELEMENT(res);
      }
    }
    else if(type == RemoteServerPacket::GetResolve)
    {
      nbdarray<uint64_t> StackAddresses;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(StackAddresses);
      }

      reader.EndChunk();

      nbdarray<nbdstr> StackFrames;

      if(resolver)
      {
        StackFrames.reserve(StackAddresses.size());
        for(uint64_t frame : StackAddresses)
        {
          Callstack::AddressDetails info = resolver->GetAddr(frame);
          StackFrames.push_back(info.formattedString());
        }
      }
      else
      {
        StackFrames = {""};
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetResolve);
        SERIALISE_ELEMENT(StackFrames);
      }
    }
    else if(type == RemoteServerPacket::GetDriverName)
    {
      reader.EndChunk();

      nbdstr driver = nbd ? nbd->GetDriverName() : "";
      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetDriverName);
        SERIALISE_ELEMENT(driver);
      }
    }
    else if(type == RemoteServerPacket::GetSectionCount)
    {
      reader.EndChunk();

      int count = nbd ? nbd->NumSections() : 0;

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetSectionCount);
        SERIALISE_ELEMENT(count);
      }
    }
    else if(type == RemoteServerPacket::FindSectionByName)
    {
      nbdstr name;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(name);
      }

      reader.EndChunk();

      int index = nbd ? nbd->SectionIndex(name) : -1;

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::FindSectionByName);
        SERIALISE_ELEMENT(index);
      }
    }
    else if(type == RemoteServerPacket::FindSectionByType)
    {
      SectionType sectionType;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(sectionType);
      }

      reader.EndChunk();

      int index = nbd ? nbd->SectionIndex(sectionType) : -1;

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::FindSectionByType);
        SERIALISE_ELEMENT(index);
      }
    }
    else if(type == RemoteServerPacket::GetSectionProperties)
    {
      int index = -1;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(index);
      }

      reader.EndChunk();

      SectionProperties props;
      if(nbd && index >= 0 && index < nbd->NumSections())
        props = nbd->GetSectionProperties(index);

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetSectionProperties);
        SERIALISE_ELEMENT(props);
      }
    }
    else if(type == RemoteServerPacket::GetSectionContents)
    {
      int index = -1;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(index);
      }

      reader.EndChunk();

      bytebuf contents;

      if(nbd && index >= 0 && index < nbd->NumSections())
      {
        StreamReader *sectionReader = nbd->ReadSection(index);

        contents.resize((size_t)sectionReader->GetSize());
        bool success = sectionReader->Read(contents.data(), sectionReader->GetSize());

        if(!success)
          contents.clear();

        delete sectionReader;
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetSectionContents);
        SERIALISE_ELEMENT(contents);
      }
    }
    else if(type == RemoteServerPacket::WriteSection)
    {
      SectionProperties props;
      bytebuf contents;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(props);
        SERIALISE_ELEMENT(contents);
      }

      reader.EndChunk();

      RDResult result;

      if(nbd)
      {
        StreamWriter *sectionWriter = nbd->WriteSection(props);

        if(sectionWriter)
        {
          sectionWriter->Write(contents.data(), contents.size());
          delete sectionWriter;
        }
        else
        {
          SET_ERROR_RESULT(result, ResultCode::FileIOFailed, "Failed to write section");
        }
      }
      else
      {
        SET_ERROR_RESULT(result, ResultCode::InternalError,
                         "Attempt to write section with no capture open");
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::WriteSection);
        SERIALISE_ELEMENT(result);
      }
    }
    else if(type == RemoteServerPacket::CloseLog)
    {
      reader.EndChunk();

      SAFE_DELETE(proxy);

      if(remoteDriver)
        remoteDriver->Shutdown();
      remoteDriver = NULL;
      replayDriver = NULL;

      SAFE_DELETE(nbd);
      SAFE_DELETE(resolver);
    }
    else if(type == RemoteServerPacket::ExecuteAndInject)
    {
      nbdstr app, workingDir, cmdLine, logfile;
      CaptureOptions opts;
      nbdarray<EnvironmentModification> env;

      {
        READ_DATA_SCOPE();
        SERIALISE_ELEMENT(app);
        SERIALISE_ELEMENT(workingDir);
        SERIALISE_ELEMENT(cmdLine);
        SERIALISE_ELEMENT(opts);
        SERIALISE_ELEMENT(env);
      }

      reader.EndChunk();

      RDResult res;
      uint32_t ident = 0;

      if(threadData->allowExecution)
      {
        nbdtie(res, ident) =
            Process::LaunchAndInjectIntoProcess(app, workingDir, cmdLine, env, "", opts, "", false);
      }
      else
      {
        NBDWARN("Requested to execute program - disallowing based on configuration");
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::ExecuteAndInject);
        SERIALISE_ELEMENT(res);
        SERIALISE_ELEMENT(ident);
      }
    }
    else if(type == RemoteServerPacket::EmbedDependenciesIntoCapture)
    {
      reader.EndChunk();

      RDResult result;
      if(nbd)
      {
        result = NoobDawn::Inst().EmbedExternalFiles(nbd);
      }
      else
      {
        SET_ERROR_RESULT(result, ResultCode::InternalError,
                         "Attempt to embed external dependency files with no capture open");
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::EmbedDependenciesIntoCapture);
        SERIALISE_ELEMENT(result);
      }
    }
    else if(type == RemoteServerPacket::RemoveDependenciesFromCapture)
    {
      reader.EndChunk();

      RDResult result;
      if(nbd)
      {
        result = NoobDawn::Inst().RemoveExternalFiles(nbd);
      }
      else
      {
        SET_ERROR_RESULT(result, ResultCode::InternalError,
                         "Attempt to remove external files with no capture open");
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::RemoveDependenciesFromCapture);
        SERIALISE_ELEMENT(result);
      }
    }
    else if(type == RemoteServerPacket::HasEmbeddedDependencies)
    {
      reader.EndChunk();

      bool res = false;
      if(nbd)
      {
        res = NoobDawn::Inst().HasEmbeddedFiles(nbd);
      }
      else
      {
        NBDWARN("Attempt to check for embedded files with no capture open");
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::HasEmbeddedDependencies);
        SERIALISE_ELEMENT(res);
      }
    }
    else if(type == RemoteServerPacket::HasPendingDependencies)
    {
      reader.EndChunk();

      bool res = false;
      if(nbd)
      {
        res = NoobDawn::Inst().HasTrackedFileData();
      }
      else
      {
        NBDWARN("Attempt to check for externally referenced files with no capture open");
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::HasPendingDependencies);
        SERIALISE_ELEMENT(res);
      }
    }
    else if(type == RemoteServerPacket::GetPendingDependenciesNicknames)
    {
      reader.EndChunk();

      nbdarray<nbdstr> res;
      if(nbd)
      {
        res = NoobDawn::Inst().GetTrackedFileNicknames();
      }
      else
      {
        NBDWARN("Attempt to get nickanmes of externally referenced files with no capture open");
      }

      {
        WRITE_DATA_SCOPE();
        SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetPendingDependenciesNicknames);
        SERIALISE_ELEMENT(res);
      }
    }
    else if((int)type >= eReplayProxy_First && proxy)
    {
      bool ok = proxy->Tick((int)type);

      if(!ok)
        break;

      continue;
    }
  }

  FileIO::logfile_close(debugLog, nbdstr());

  SAFE_DELETE(proxy);

  if(remoteDriver)
    remoteDriver->Shutdown();
  remoteDriver = NULL;
  replayDriver = NULL;
  SAFE_DELETE(nbd);
  SAFE_DELETE(resolver);

  for(size_t i = 0; i < tempFiles.size(); i++)
  {
    FileIO::Delete(tempFiles[i]);
  }

  NBDLOG("Closing active connection from %u.%u.%u.%u.", Network::GetIPOctet(ip, 0),
         Network::GetIPOctet(ip, 1), Network::GetIPOctet(ip, 2), Network::GetIPOctet(ip, 3));

  NBDLOG("Ready for new active connection...");

  SAFE_DELETE(client);
}

void NoobDawn::BecomeRemoteServer(const nbdstr &listenhost, uint16_t port,
                                   std::function<bool()> killReplay,
                                   NOOBDAWN_PreviewWindowCallback previewWindow)
{
  Network::Socket *sock = Network::CreateServerSocket(listenhost, port, 1);

  if(sock == NULL)
    return;

  nbdarray<nbdpair<uint32_t, uint32_t> > listenRanges;
  bool allowExecution = true;

  FILE *f = FileIO::fopen(FileIO::GetAppFolderFilename("remoteserver.conf"), FileIO::ReadText);

  nbdstr configFile;

  if(f)
  {
    FileIO::fseek64(f, 0, SEEK_END);
    configFile.resize((size_t)FileIO::ftell64(f));
    FileIO::fseek64(f, 0, SEEK_SET);

    FileIO::fread(configFile.data(), 1, configFile.size(), f);

    FileIO::fclose(f);
  }

  nbdarray<nbdstr> lines;
  split(configFile, lines, '\n');

  for(nbdstr &line : lines)
  {
    line.trim();

    if(line == "")
      continue;

    // skip comments
    if(line[0] == '#')
      continue;

    if(line.substr(0, sizeof("whitelist") - 1) == "whitelist")
    {
      uint32_t ip = 0, mask = 0;

      // CIDR notation
      bool found = Network::ParseIPRangeCIDR(line.substr(sizeof("whitelist")), ip, mask);

      if(found)
      {
        listenRanges.push_back(make_nbdpair(ip, mask));
        continue;
      }
      else
      {
        NBDLOG("Couldn't parse IP range from: %s", line.c_str() + sizeof("whitelist"));
      }

      continue;
    }
    else if(line.substr(0, sizeof("noexec") - 1) == "noexec")
    {
      allowExecution = false;

      continue;
    }

    NBDLOG("Malformed line '%s'. See documentation for file format.", line.c_str());
  }

  if(listenRanges.empty())
  {
    NBDLOG("No whitelist IP ranges configured - using default private IP ranges.");
    NBDLOG(
        "Create a config file remoteserver.conf in ~/.noobdawn or %%APPDATA%%/noobdawn to "
        "narrow "
        "this down or accept connections from more ranges.");

    listenRanges.push_back(make_nbdpair(Network::MakeIP(10, 0, 0, 0), 0xff000000));
    listenRanges.push_back(make_nbdpair(Network::MakeIP(172, 16, 0, 0), 0xfff00000));
    listenRanges.push_back(make_nbdpair(Network::MakeIP(192, 168, 0, 0), 0xffff0000));
  }

  NBDLOG("Allowing connections from:");

  for(size_t i = 0; i < listenRanges.size(); i++)
  {
    uint32_t ip = listenRanges[i].first;
    uint32_t mask = listenRanges[i].second;

    NBDLOG("%u.%u.%u.%u / %u.%u.%u.%u", Network::GetIPOctet(ip, 0), Network::GetIPOctet(ip, 1),
           Network::GetIPOctet(ip, 2), Network::GetIPOctet(ip, 3), Network::GetIPOctet(mask, 0),
           Network::GetIPOctet(mask, 1), Network::GetIPOctet(mask, 2), Network::GetIPOctet(mask, 3));
  }

  if(allowExecution)
    NBDLOG("Allowing execution commands");
  else
    NBDLOG("Blocking execution commands");

  NBDLOG("Replay host ready for requests...");

  ActiveClient activeClientData;

  nbdarray<ClientThread *> clients;

  while(!killReplay())
  {
    Network::Socket *client = sock->AcceptClient(0);

    {
      SCOPED_LOCK(activeClientData.lock);
      if(activeClientData.active && activeClientData.active->killServer)
        break;
    }

    // reap any dead client threads
    for(size_t i = 0; i < clients.size(); i++)
    {
      if(clients[i]->socket == NULL)
      {
        {
          SCOPED_LOCK(activeClientData.lock);
          if(activeClientData.active == clients[i])
            activeClientData.active = NULL;
        }

        Threading::JoinThread(clients[i]->thread);
        Threading::CloseThread(clients[i]->thread);
        delete clients[i];
        clients.erase(i);
        break;
      }
    }

    if(client == NULL)
    {
      if(!sock->Connected())
      {
        NBDERR("Error in accept - shutting down server");

        SAFE_DELETE(sock);
        return;
      }

      Threading::Sleep(5);

      continue;
    }

    uint32_t ip = client->GetRemoteIP();

    NBDLOG("Connection received from %u.%u.%u.%u.", Network::GetIPOctet(ip, 0),
           Network::GetIPOctet(ip, 1), Network::GetIPOctet(ip, 2), Network::GetIPOctet(ip, 3));

    bool valid = false;

    // always allow connections from localhost
    valid = Network::MatchIPMask(ip, Network::MakeIP(127, 0, 0, 1), ~0U);

    for(size_t i = 0; i < listenRanges.size(); i++)
    {
      if(Network::MatchIPMask(ip, listenRanges[i].first, listenRanges[i].second))
      {
        valid = true;
        break;
      }
    }

    if(!valid)
    {
      NBDLOG("Doesn't match any listen range, closing connection.");
      SAFE_DELETE(client);
      continue;
    }

    NBDLOG("Processing connection");

    ClientThread *clientThread = new ClientThread();
    clientThread->socket = client;
    clientThread->allowExecution = allowExecution;
    clientThread->thread =
        Threading::CreateThread([&activeClientData, clientThread, previewWindow]() {
          if(HandleHandshakeClient(activeClientData, clientThread))
          {
            ActiveRemoteClientThread(clientThread, previewWindow);
          }
          else
          {
            SAFE_DELETE(clientThread->socket);
          }
        });

    clients.push_back(clientThread);
  }

  {
    SCOPED_LOCK(activeClientData.lock);
    if(activeClientData.active)
      activeClientData.active->killThread = true;
    activeClientData.active = NULL;
  }

  // shut down client threads
  for(size_t i = 0; i < clients.size(); i++)
  {
    Threading::JoinThread(clients[i]->thread);
    Threading::CloseThread(clients[i]->thread);
    delete clients[i];
  }

  SAFE_DELETE(sock);
}

extern "C" NOOBDAWN_API ResultDetails NOOBDAWN_CC
NOOBDAWN_CreateRemoteServerConnection(const nbdstr &URL, IRemoteServer **rend)
{
  nbdstr host = "localhost";
  if(!URL.empty())
    host = URL;

  nbdstr deviceID = host;

  IDeviceProtocolHandler *protocol = NoobDawn::Inst().GetDeviceProtocol(deviceID);

  uint16_t port = NoobDawn_RemoteServerPort;

  if(protocol)
  {
    deviceID = protocol->GetDeviceID(deviceID);
    host = protocol->RemapHostname(deviceID);
    if(host.empty())
      return RDResult(ResultCode::NetworkIOFailed);

    port = protocol->RemapPort(deviceID, port);
  }
  else
  {
    int32_t idx = deviceID.indexOf(':');
    if(idx > 0)
    {
      host = deviceID.substr(0, idx);
      port = atoi(deviceID.substr(idx + 1).c_str()) & 0xffff;
    }
  }

  if(port == 0)
    return RDResult(ResultCode::NetworkIOFailed);

  Network::Socket *sock = Network::CreateClientSocket(host, port, 750);

  if(sock == NULL)
    return RDResult(ResultCode::NetworkIOFailed);

  uint32_t version = RemoteServerProtocolVersion;

  sock->SetTimeout(RemoteServer_TimeoutMS());

  bool activeConnection = (rend != NULL);

  {
    WriteSerialiser ser(new StreamWriter(sock, Ownership::Nothing), Ownership::Stream);

    ser.SetStreamingMode(true);

    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::Handshake);
    SERIALISE_ELEMENT(version);
    SERIALISE_ELEMENT(activeConnection);
  }

  if(!sock->Connected())
    return RDResult(ResultCode::NetworkIOFailed);

  {
    ReadSerialiser ser(new StreamReader(sock, Ownership::Nothing), Ownership::Stream);

    ser.SetStreamingMode(true);

    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    uint32_t remoteVersion = 0;
    if(type == RemoteServerPacket::VersionMismatch2)
    {
      SERIALISE_ELEMENT(remoteVersion);
    }

    ser.EndChunk();

    if(type == RemoteServerPacket::Busy)
    {
      SAFE_DELETE(sock);
      return RDResult(ResultCode::NetworkRemoteBusy);
    }

    if(type == RemoteServerPacket::VersionMismatch || type == RemoteServerPacket::VersionMismatch2)
    {
      SAFE_DELETE(sock);

      nbdstr ver = StringFormat::Fmt("Server on v%d.%d", remoteVersion / 1000, remoteVersion % 1000);

      if(remoteVersion == 0)
        ver = "Server older than v1.23";

      return RDResult(ResultCode::NetworkVersionMismatch, ver);
    }

    if(ser.IsErrored() || type != RemoteServerPacket::Handshake)
    {
      NBDWARN("Didn't get proper handshake");
      SAFE_DELETE(sock);
      return RDResult(ResultCode::NetworkIOFailed);
    }
  }

  if(rend == NULL)
  {
    SAFE_DELETE(sock);
    return RDResult(ResultCode::Succeeded);
  }

  if(protocol)
    *rend = protocol->CreateRemoteServer(sock, deviceID);
  else
    *rend = new RemoteServer(sock, deviceID);

  if(*rend == NULL)
    return RDResult(ResultCode::NetworkIOFailed);

  return RDResult(ResultCode::Succeeded);
}

extern "C" NOOBDAWN_API ResultDetails NOOBDAWN_CC
NOOBDAWN_CheckRemoteServerConnection(const nbdstr &URL)
{
  return NOOBDAWN_CreateRemoteServerConnection(URL, NULL);
}

#undef WRITE_DATA_SCOPE
#undef READ_DATA_SCOPE
#define WRITE_DATA_SCOPE() WriteSerialiser &ser = *writer;
#define READ_DATA_SCOPE() ReadSerialiser &ser = *reader;

RemoteServer::RemoteServer(Network::Socket *sock, const nbdstr &deviceID)
    : m_Socket(sock), m_deviceID(deviceID)
{
  reader = new ReadSerialiser(new StreamReader(sock, Ownership::Nothing), Ownership::Stream);
  writer = new WriteSerialiser(new StreamWriter(sock, Ownership::Nothing), Ownership::Stream);

  if(RemoteServer_DebugLogging())
  {
    reader->ConfigureStructuredExport(&GetRemoteServerChunkName, false, 0, 1.0);
    writer->ConfigureStructuredExport(&GetRemoteServerChunkName, false, 0, 1.0);

    nbdstr filename = FileIO::GetTempFolderFilename() + "/NoobDawn/RemoteServer_Client.log";

    NBDLOG("Logging remote server work to '%s'", filename.c_str());

    // truncate the log
    debugLog = FileIO::logfile_open(filename);
    FileIO::logfile_close(debugLog, filename);
    debugLog = FileIO::logfile_open(filename);

    reader->EnableDumping(debugLog);
    writer->EnableDumping(debugLog);
  }
  else
  {
    debugLog = NULL;
  }

  writer->SetStreamingMode(true);
  reader->SetStreamingMode(true);

  std::map<NBDDriver, nbdstr> m = NoobDawn::Inst().GetReplayDrivers();

  m_Proxies.reserve(m.size());
  for(auto it = m.begin(); it != m.end(); ++it)
    m_Proxies.push_back({it->first, it->second});
}

RemoteServer::~RemoteServer()
{
  FileIO::logfile_close(debugLog, nbdstr());
  SAFE_DELETE(writer);
  SAFE_DELETE(reader);
  SAFE_DELETE(m_Socket);
}

void RemoteServer::ShutdownConnection()
{
  delete this;
}

void RemoteServer::ShutdownServerAndConnection()
{
  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::ShutdownServer);
  }

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();
    ser.EndChunk();

    NBDASSERT(type == RemoteServerPacket::ShutdownServer);
  }

  delete this;
}

bool RemoteServer::Connected()
{
  return m_Socket != NULL && m_Socket->Connected();
}

ResultDetails RemoteServer::Ping()
{
  RDResult ret;

  if(!Connected())
  {
    ret = ResultCode::RemoteServerConnectionLost;
    return ret;
  }

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::Ping);
  }

  RemoteServerPacket type;

  {
    READ_DATA_SCOPE();
    type = ser.ReadChunk<RemoteServerPacket>();
    ser.EndChunk();
  }

  if(type == RemoteServerPacket::Ping)
    ret = ResultCode::Succeeded;
  else
    ret = ResultCode::RemoteServerConnectionLost;

  return ret;
}

nbdarray<nbdstr> RemoteServer::LocalProxies()
{
  nbdarray<nbdstr> out;

  m_Proxies.reserve(m_Proxies.size());

  size_t i = 0;
  for(auto it = m_Proxies.begin(); it != m_Proxies.end(); ++it, ++i)
    out.push_back(it->second);

  return out;
}

nbdarray<nbdstr> RemoteServer::RemoteSupportedReplays()
{
  nbdarray<nbdstr> out;

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::RemoteDriverList);
  }

  {
    READ_DATA_SCOPE();

    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::RemoteDriverList)
    {
      uint32_t count = 0;
      SERIALISE_ELEMENT(count);

      out.reserve(count);

      for(uint32_t i = 0; i < count; i++)
      {
        NBDDriver driverType = NBDDriver::Unknown;
        nbdstr driverName = "";

        SERIALISE_ELEMENT(driverType);
        SERIALISE_ELEMENT(driverName);

        out.push_back(driverName);
      }
    }
    else
    {
      NBDERR("Unexpected response to remote driver list request");
    }

    ser.EndChunk();
  }

  return out;
}

nbdstr RemoteServer::GetHomeFolder()
{
  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::HomeDir);
  }

  nbdstr home;

  {
    READ_DATA_SCOPE();

    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::HomeDir)
    {
      SERIALISE_ELEMENT(home);
    }
    else
    {
      NBDERR("Unexpected response to home folder request");
    }

    ser.EndChunk();
  }

  return home;
}

nbdarray<PathEntry> RemoteServer::ListFolder(const nbdstr &path)
{
  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::ListDir);
    SERIALISE_ELEMENT(path);
  }

  nbdarray<PathEntry> files;

  {
    READ_DATA_SCOPE();

    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::ListDir)
    {
      SERIALISE_ELEMENT(files);
    }
    else
    {
      NBDERR("Unexpected response to list directory request");
      files.resize(1);
      files[0].filename = path;
      files[0].flags = PathProperty::ErrorUnknown;
    }

    ser.EndChunk();
  }

  return files;
}

ExecuteResult RemoteServer::ExecuteAndInject(const nbdstr &app, const nbdstr &workingDir,
                                             const nbdstr &cmdline,
                                             const nbdarray<EnvironmentModification> &env,
                                             const CaptureOptions &opts, const nbdstr &blacklist)
{
  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::ExecuteAndInject);
    SERIALISE_ELEMENT(app);
    SERIALISE_ELEMENT(workingDir);
    SERIALISE_ELEMENT(cmdline);
    SERIALISE_ELEMENT(opts);
    SERIALISE_ELEMENT(env);
  }

  ExecuteResult ret = {};

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::ExecuteAndInject)
    {
      SERIALISE_ELEMENT_LOCAL(result, RDResult());
      SERIALISE_ELEMENT_LOCAL(ident, uint32_t());

      ret.result = result;
      ret.ident = ident;
    }
    else
    {
      NBDERR("Unexpected response to execute and inject request");
    }

    ser.EndChunk();
  }

  return ret;
}

void RemoteServer::CopyCaptureFromRemote(const nbdstr &remotepath, const nbdstr &localpath,
                                         NOOBDAWN_ProgressCallback progress)
{
  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::CopyCaptureFromRemote);
    SERIALISE_ELEMENT(remotepath);
  }

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::CopyCaptureFromRemote)
    {
      StreamWriter streamWriter(FileIO::fopen(localpath, FileIO::WriteBinary), Ownership::Stream);

      ser.SerialiseStream(localpath, streamWriter, progress);

      if(ser.IsErrored())
      {
        NBDERR("Network error receiving file");
        return;
      }
    }
    else
    {
      NBDERR("Unexpected response to capture copy request");
    }

    ser.EndChunk();
  }
}

nbdstr RemoteServer::CopyCaptureToRemote(const nbdstr &filename, NOOBDAWN_ProgressCallback progress)
{
  FILE *fileHandle = FileIO::fopen(filename, FileIO::ReadBinary);

  if(!fileHandle)
  {
    NBDERR("Can't open file '%s'", filename.c_str());
    return "";
  }

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::CopyCaptureToRemote);

    // this will take ownership of and close the file
    StreamReader fileStream(fileHandle);
    ser.SerialiseStream(filename, fileStream, progress);
  }

  nbdstr path;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::CopyCaptureToRemote)
    {
      SERIALISE_ELEMENT(path);
    }
    else
    {
      NBDERR("Unexpected response to capture copy request");
    }

    ser.EndChunk();
  }

  return path;
}

void RemoteServer::TakeOwnershipCapture(const nbdstr &filename)
{
  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::TakeOwnershipCapture);
    SERIALISE_ELEMENT(filename);
  }
}

nbdpair<ResultDetails, IReplayController *> RemoteServer::OpenCapture(
    uint32_t proxyid, const nbdstr &filename, const ReplayOptions &opts,
    NOOBDAWN_ProgressCallback progress)
{
  nbdpair<ResultDetails, IReplayController *> ret;
  ret.first = ResultCode::InternalError;
  ret.second = NULL;

  if(proxyid != ~0U && proxyid >= m_Proxies.size())
  {
    NBDERR("Invalid proxy driver id %d specified for remote renderer", proxyid);
    return ret;
  }

  NBDLOG("Opening capture remotely");

  LogReplayOptions(opts);

  // if the proxy id is ~0U, then we just don't care so let NoobDawn pick the most
  // appropriate supported proxy for the current platform.
  NBDDriver proxydrivertype = proxyid == ~0U ? NBDDriver::Unknown : m_Proxies[proxyid].first;

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::OpenLog);
    SERIALISE_ELEMENT(filename);
    SERIALISE_ELEMENT(opts);
  }

  RemoteServerPacket type = RemoteServerPacket::Noop;
  while(!reader->IsErrored())
  {
    READ_DATA_SCOPE();
    type = ser.ReadChunk<RemoteServerPacket>();

    if(reader->IsErrored() || type != RemoteServerPacket::LogOpenProgress)
      break;

    float progressValue = 0.0f;

    SERIALISE_ELEMENT(progressValue);

    ser.EndChunk();

    if(progress)
      progress(progressValue);
  }

  NBDLOG("Capture open complete");

  if(reader->IsErrored() || type != RemoteServerPacket::LogOpened)
  {
    NBDERR("Error opening capture");
    ret.first = ResultCode::NetworkIOFailed;
    return ret;
  }

  RDResult result = ResultCode::Succeeded;
  {
    READ_DATA_SCOPE();
    SERIALISE_ELEMENT(result);
    ser.EndChunk();
  }

  if(progress)
    progress(1.0f);

  if(result != ResultCode::Succeeded)
  {
    NBDERR("Capture open failed: %s", ResultDetails(result).Message().c_str());
    ret.first = result;
    return ret;
  }

  NBDLOG("Capture ready on replay host");

  IReplayDriver *proxyDriver = NULL;
  result = NoobDawn::Inst().CreateProxyReplayDriver(proxydrivertype, &proxyDriver);

  if(result != ResultCode::Succeeded || !proxyDriver)
  {
    NBDERR("Creating proxy driver failed: %s", ResultDetails(result).Message().c_str());
    if(proxyDriver)
      proxyDriver->Shutdown();
    ret.first = result;
    return ret;
  }

  ReplayController *rend = new ReplayController();

  ReplayProxy *proxy = new ReplayProxy(*reader, *writer, proxyDriver);
  result = rend->SetDevice(proxy);

  if(result != ResultCode::Succeeded)
  {
    rend->Shutdown();
    ret.first = result;
    return ret;
  }

  // ReplayController takes ownership of the ProxySerialiser (as IReplayDriver)
  // and it cleans itself up in Shutdown.

  NBDLOG("Remote capture open complete & proxy ready");

  ret.first = ResultCode::Succeeded;
  ret.second = rend;
  return ret;
}

void RemoteServer::CloseCapture(IReplayController *rend)
{
  rend->Shutdown();

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::CloseLog);
  }
}

nbdstr RemoteServer::DriverName()
{
  if(!Connected())
    return "";

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetDriverName);
  }

  nbdstr driverName = "";

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::GetDriverName)
    {
      SERIALISE_ELEMENT(driverName);
    }
    else
    {
      NBDERR("Unexpected response to GetDriverName");
    }

    ser.EndChunk();
  }

  return driverName;
}

nbdarray<GPUDevice> RemoteServer::GetAvailableGPUs()
{
  if(!Connected())
    return {};

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetAvailableGPUs);
  }

  nbdarray<GPUDevice> gpus;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::GetAvailableGPUs)
    {
      SERIALISE_ELEMENT(gpus);
    }
    else
    {
      NBDERR("Unexpected response to GetAvailableGPUs");
    }

    ser.EndChunk();
  }

  return gpus;
}

int RemoteServer::GetSectionCount()
{
  if(!Connected())
    return 0;

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetSectionCount);
  }

  int count = 0;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::GetSectionCount)
    {
      SERIALISE_ELEMENT(count);
    }
    else
    {
      NBDERR("Unexpected response to GetSectionCount");
    }

    ser.EndChunk();
  }

  return count;
}

int RemoteServer::FindSectionByName(const nbdstr &name)
{
  if(!Connected())
    return -1;

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::FindSectionByName);
    SERIALISE_ELEMENT(name);
  }

  int index = -1;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::FindSectionByName)
    {
      SERIALISE_ELEMENT(index);
    }
    else
    {
      NBDERR("Unexpected response to FindSectionByName");
    }

    ser.EndChunk();
  }

  return index;
}

int RemoteServer::FindSectionByType(SectionType sectionType)
{
  if(!Connected())
    return -1;

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::FindSectionByType);
    SERIALISE_ELEMENT(sectionType);
  }

  int index = -1;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::FindSectionByType)
    {
      SERIALISE_ELEMENT(index);
    }
    else
    {
      NBDERR("Unexpected response to FindSectionByType");
    }

    ser.EndChunk();
  }

  return index;
}

SectionProperties RemoteServer::GetSectionProperties(int index)
{
  if(!Connected())
    return SectionProperties();

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetSectionProperties);
    SERIALISE_ELEMENT(index);
  }

  SectionProperties props;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::GetSectionProperties)
    {
      SERIALISE_ELEMENT(props);
    }
    else
    {
      NBDERR("Unexpected response to GetSectionProperties");
    }

    ser.EndChunk();
  }

  return props;
}

bytebuf RemoteServer::GetSectionContents(int index)
{
  if(!Connected())
    return bytebuf();

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetSectionContents);
    SERIALISE_ELEMENT(index);
  }

  bytebuf contents;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::GetSectionContents)
    {
      SERIALISE_ELEMENT(contents);
    }
    else
    {
      NBDERR("Unexpected response to GetSectionContents");
    }

    ser.EndChunk();
  }

  return contents;
}

ResultDetails RemoteServer::WriteSection(const SectionProperties &props, const bytebuf &contents)
{
  RDResult ret;

  if(!Connected())
  {
    ret.code = ResultCode::RemoteServerConnectionLost;
    return ret;
  }

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::WriteSection);
    SERIALISE_ELEMENT(props);
    SERIALISE_ELEMENT(contents);
  }

  RDResult success;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::WriteSection)
    {
      SERIALISE_ELEMENT(success);
    }
    else
    {
      NBDERR("Unexpected response to write section request");
    }

    ser.EndChunk();
  }

  return success;
}

bool RemoteServer::HasCallstacks()
{
  if(!Connected())
    return false;

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::HasCallstacks);
  }

  bool hasCallstacks = false;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::HasCallstacks)
    {
      SERIALISE_ELEMENT(hasCallstacks);
    }
    else
    {
      NBDERR("Unexpected response to has callstacks request");
    }

    ser.EndChunk();
  }

  return hasCallstacks;
}

ResultDetails RemoteServer::InitResolver(bool interactive, NOOBDAWN_ProgressCallback progress)
{
  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::InitResolver);
  }

  RemoteServerPacket type = RemoteServerPacket::Noop;
  while(!reader->IsErrored())
  {
    READ_DATA_SCOPE();
    type = ser.ReadChunk<RemoteServerPacket>();

    if(reader->IsErrored() || type != RemoteServerPacket::ResolverProgress)
      break;

    float progressValue = 0.0f;

    SERIALISE_ELEMENT(progressValue);

    ser.EndChunk();

    if(progress)
      progress(progressValue);

    NBDLOG("% 3.0f%%...", progressValue * 100.0f);
  }

  RDResult res;

  if(reader->IsErrored() || type != RemoteServerPacket::InitResolver)
  {
    res = ResultCode::NetworkIOFailed;
    return res;
  }

  {
    READ_DATA_SCOPE();
    SERIALISE_ELEMENT(res);
    ser.EndChunk();
  }

  if(progress)
    progress(1.0f);

  return res;
}

nbdarray<nbdstr> RemoteServer::GetResolve(const nbdarray<uint64_t> &callstack)
{
  if(!Connected())
    return {""};

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetResolve);
    SERIALISE_ELEMENT(callstack);
  }

  nbdarray<nbdstr> StackFrames;

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::GetResolve)
    {
      SERIALISE_ELEMENT(StackFrames);
    }
    else
    {
      NBDERR("Unexpected response to resolve request");
    }

    ser.EndChunk();
  }

  return StackFrames;
}

ResultDetails RemoteServer::EmbedDependenciesIntoCapture()
{
  RDResult ret;

  if(!Connected())
  {
    ret.code = ResultCode::RemoteServerConnectionLost;
    return ret;
  }

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::EmbedDependenciesIntoCapture);
  }

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::EmbedDependenciesIntoCapture)
    {
      SERIALISE_ELEMENT(ret);
    }
    else
    {
      NBDERR("Unexpected response to embed dependencies into capture request");
    }

    ser.EndChunk();
  }

  return ret;
}

ResultDetails RemoteServer::RemoveDependenciesFromCapture()
{
  RDResult ret;
  if(!Connected())
  {
    ret.code = ResultCode::RemoteServerConnectionLost;
    return ret;
  }

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::RemoveDependenciesFromCapture);
  }

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::RemoveDependenciesFromCapture)
    {
      SERIALISE_ELEMENT(ret);
    }
    else
    {
      NBDERR("Unexpected response to remove dependencies from capture request");
    }

    ser.EndChunk();
  }

  return ret;
}

bool RemoteServer::HasEmbeddedDependencies()
{
  bool ret = false;
  if(!Connected())
  {
    return false;
  }

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::HasEmbeddedDependencies);
  }

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::HasEmbeddedDependencies)
    {
      SERIALISE_ELEMENT(ret);
    }
    else
    {
      NBDERR("Unexpected response to has embedded dependencies request");
    }

    ser.EndChunk();
  }
  return ret;
}

bool RemoteServer::HasPendingDependencies()
{
  bool ret = false;
  if(!Connected())
  {
    return false;
  }

  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::HasPendingDependencies);
  }

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::HasPendingDependencies)
    {
      SERIALISE_ELEMENT(ret);
    }
    else
    {
      NBDERR("Unexpected response to has pending dependencies request");
    }

    ser.EndChunk();
  }
  return ret;
}

nbdarray<nbdstr> RemoteServer::GetPendingDependenciesNicknames()
{
  nbdarray<nbdstr> ret;
  if(!Connected())
  {
    return ret;
  }
  {
    WRITE_DATA_SCOPE();
    SCOPED_SERIALISE_CHUNK(RemoteServerPacket::GetPendingDependenciesNicknames);
  }

  {
    READ_DATA_SCOPE();
    RemoteServerPacket type = ser.ReadChunk<RemoteServerPacket>();

    if(type == RemoteServerPacket::GetPendingDependenciesNicknames)
    {
      SERIALISE_ELEMENT(ret);
    }
    else
    {
      NBDERR("Unexpected response to get nicknmes of externally referenced files");
    }

    ser.EndChunk();
  }
  return ret;
}
