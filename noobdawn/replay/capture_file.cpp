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

#include "core/core.h"
#include "jpeg-compressor/jpgd.h"
#include "jpeg-compressor/jpge.h"
#include "replay/replay_controller.h"
#include "serialise/nbdfile.h"
#include "serialise/serialiser.h"
#include "stb/stb_image.h"
#include "stb/stb_image_resize2.h"
#include "stb/stb_image_write.h"

static void writeToBytebuf(void *context, void *data, int size)
{
  bytebuf *buf = (bytebuf *)context;
  buf->append((byte *)data, size);
}

static NBDDriver driverFromName(const nbdstr &driverName)
{
  for(int d = (int)NBDDriver::Unknown; d < (int)NBDDriver::MaxBuiltin; d++)
  {
    if(driverName == ToStr((NBDDriver)d))
      return (NBDDriver)d;
  }

  return NBDDriver::Unknown;
}

static NBDThumb convertThumb(FileType thumbType, uint32_t thumbWidth, uint32_t thumbHeight,
                             const bytebuf &thumbData)
{
  NBDThumb ret;

  if(thumbWidth > 0xffff || thumbHeight > 0xffff)
    return ret;

  ret.width = thumbWidth & 0xffff;
  ret.height = thumbHeight & 0xffff;

  byte *decoded = NULL;

  if(thumbType == FileType::JPG)
  {
    ret.pixels = thumbData;
  }
  else
  {
    int ignore = 0;
    decoded =
        stbi_load_from_memory(thumbData.data(), thumbData.count(), &ignore, &ignore, &ignore, 3);

    if(decoded == NULL)
    {
      NBDERR("Couldn't decode provided thumbnail");
      return ret;
    }
  }

  if(decoded)
  {
    int len = ret.width * ret.height * 3;
    ret.pixels.resize(len);

    jpge::params p;
    p.m_quality = 90;
    jpge::compress_image_to_jpeg_file_in_memory(ret.pixels.data(), len, (int)ret.width,
                                                (int)ret.height, 3, decoded, p);

    ret.pixels.resize(len);

    free(decoded);
  }

  return ret;
}

class CaptureFile : public ICaptureFile
{
public:
  CaptureFile();
  virtual ~CaptureFile();

  ResultDetails OpenFile(const nbdstr &filename, const nbdstr &filetype,
                         NOOBDAWN_ProgressCallback progress);
  ResultDetails OpenBuffer(const bytebuf &buffer, const nbdstr &filetype,
                           NOOBDAWN_ProgressCallback progress);
  ResultDetails CopyFileTo(const nbdstr &filename);
  void Shutdown() { delete this; }
  ReplaySupport LocalReplaySupport() { return m_Support; }
  nbdstr DriverName() { return m_DriverName; }
  nbdstr RecordedMachineIdent() { return m_Ident; }
  uint64_t TimestampBase() { return m_NBD ? m_NBD->GetTimestampBase() : 0; }
  double TimestampFrequency() { return m_NBD ? m_NBD->GetTimestampFrequency() : 1.0; }
  nbdpair<ResultDetails, IReplayController *> OpenCapture(const ReplayOptions &opts,
                                                          NOOBDAWN_ProgressCallback progress);

  void SetMetadata(const nbdstr &driverName, uint64_t machineIdent, FileType thumbType,
                   uint32_t thumbWidth, uint32_t thumbHeight, const bytebuf &thumbData,
                   uint64_t timeBase, double timeFreq);

  ResultDetails Convert(const nbdstr &filename, const nbdstr &filetype, const SDFile *file,
                        NOOBDAWN_ProgressCallback progress);

  nbdarray<CaptureFileFormat> GetCaptureFileFormats()
  {
    return NoobDawn::Inst().GetCaptureFileFormats();
  }

  nbdarray<GPUDevice> GetAvailableGPUs() { return NoobDawn::Inst().GetAvailableGPUs(); }
  const SDFile &GetStructuredData()
  {
    // decompile to structured data on demand.
    InitStructuredData();

    return m_StructuredData;
  }

  void SetStructuredData(const SDFile &file)
  {
    m_StructuredData.version = file.version;

    m_StructuredData.chunks.reserve(file.chunks.size());

    for(SDChunk *obj : file.chunks)
      m_StructuredData.chunks.push_back(obj->Duplicate());

    m_StructuredData.buffers.reserve(file.buffers.size());

    for(bytebuf *buf : file.buffers)
      m_StructuredData.buffers.push_back(new bytebuf(*buf));
  }

  Thumbnail GetThumbnail(FileType type, uint32_t maxsize);

  // ICaptureAccess

  int32_t GetSectionCount();
  int32_t FindSectionByName(const nbdstr &name);
  int32_t FindSectionByType(SectionType type);
  SectionProperties GetSectionProperties(int32_t index);
  bytebuf GetSectionContents(int32_t index);
  ResultDetails WriteSection(const SectionProperties &props, const bytebuf &contents);

  bool HasCallstacks();
  ResultDetails InitResolver(bool interactive, NOOBDAWN_ProgressCallback progress);
  nbdarray<nbdstr> GetResolve(const nbdarray<uint64_t> &callstack);

  ResultDetails EmbedDependenciesIntoCapture();
  ResultDetails RemoveDependenciesFromCapture();
  bool HasEmbeddedDependencies();
  bool HasPendingDependencies();
  nbdarray<nbdstr> GetPendingDependenciesNicknames();

private:
  ResultDetails Init();

  RDResult InitStructuredData(NOOBDAWN_ProgressCallback progress = NOOBDAWN_ProgressCallback());

  NBDFile *m_NBD = NULL;
  Callstack::StackResolver *m_Resolver = NULL;

  SDFile m_StructuredData;

  nbdstr m_DriverName, m_Ident;
  ReplaySupport m_Support = ReplaySupport::Unsupported;
};

CaptureFile::CaptureFile()
{
}

CaptureFile::~CaptureFile()
{
  SAFE_DELETE(m_NBD);
  SAFE_DELETE(m_Resolver);
}

ResultDetails CaptureFile::OpenFile(const nbdstr &filename, const nbdstr &filetype,
                                    NOOBDAWN_ProgressCallback progress)
{
  CaptureImporter importer = NoobDawn::Inst().GetCaptureImporter(filetype);

  if(importer)
  {
    ResultDetails ret;

    {
      StreamReader reader(FileIO::fopen(filename, FileIO::ReadBinary));
      SAFE_DELETE(m_NBD);
      m_NBD = new NBDFile;
      ret = importer(filename, reader, m_NBD, m_StructuredData, progress);
    }

    if(ret.code != ResultCode::Succeeded)
    {
      SAFE_DELETE(m_NBD);
      return ret;
    }
  }
  else
  {
    if(filetype != "" && filetype != "nbd")
      NBDWARN("Opening file with unrecognised filetype '%s' - treating as 'nbd'", filetype.c_str());

    if(progress)
      progress(0.0f);

    SAFE_DELETE(m_NBD);
    m_NBD = new NBDFile;
    m_NBD->Open(filename);

    if(progress)
      progress(1.0f);
  }

  return Init();
}

ResultDetails CaptureFile::OpenBuffer(const bytebuf &buffer, const nbdstr &filetype,
                                      NOOBDAWN_ProgressCallback progress)
{
  CaptureImporter importer = NoobDawn::Inst().GetCaptureImporter(filetype);

  if(importer)
  {
    RDResult ret;

    {
      StreamReader reader(buffer);
      SAFE_DELETE(m_NBD);
      m_NBD = new NBDFile;
      ret = importer(nbdstr(), reader, m_NBD, m_StructuredData, progress);
    }

    if(ret != ResultCode::Succeeded)
    {
      SAFE_DELETE(m_NBD);
      return ret;
    }
  }
  else
  {
    if(filetype != "" && filetype != "nbd")
      NBDWARN("Opening file with unrecognised filetype '%s' - treating as 'nbd'", filetype.c_str());

    if(progress)
      progress(0.0f);

    SAFE_DELETE(m_NBD);
    m_NBD = new NBDFile;
    m_NBD->Open(buffer);

    if(progress)
      progress(1.0f);
  }

  return Init();
}

ResultDetails CaptureFile::CopyFileTo(const nbdstr &filename)
{
  if(m_NBD)
    return m_NBD->CopyFileTo(filename);

  return RDResult(ResultCode::InternalError, "NBD file unexpectedly NULL");
}

ResultDetails CaptureFile::Init()
{
  if(!m_NBD)
    return RDResult(ResultCode::InternalError, "NBD file unexpectedly NULL");

  RDResult nbdRes = m_NBD->Error();

  if(nbdRes != ResultCode::Succeeded)
    return nbdRes;

  NBDDriver driverType = m_NBD->GetDriver();
  m_DriverName = m_NBD->GetDriverName();

  uint64_t fileMachineIdent = m_NBD->GetMachineIdent();

  m_Support = NoobDawn::Inst().HasReplayDriver(driverType) ? ReplaySupport::Supported
                                                            : ReplaySupport::Unsupported;

  if(fileMachineIdent != 0)
  {
    uint64_t machineIdent = OSUtility::GetMachineIdent();

    m_Ident = OSUtility::MakeMachineIdentString(fileMachineIdent);

    if((machineIdent & OSUtility::MachineIdent_OS_Mask) !=
       (fileMachineIdent & OSUtility::MachineIdent_OS_Mask))
      m_Support = ReplaySupport::SuggestRemote;
  }

  // can't open files without a capture in them (except images, which are special)
  if(driverType != NBDDriver::Image && m_NBD->SectionIndex(SectionType::FrameCapture) == -1)
    m_Support = ReplaySupport::Unsupported;

  return RDResult();
}

RDResult CaptureFile::InitStructuredData(NOOBDAWN_ProgressCallback progress)
{
  if(m_StructuredData.chunks.empty())
  {
    if(m_NBD && m_NBD->SectionIndex(SectionType::FrameCapture) >= 0)
    {
      StructuredProcessor proc = NoobDawn::Inst().GetStructuredProcessor(m_NBD->GetDriver());

      NoobDawn::Inst().SetProgressCallback<LoadProgress>(progress);

      RDResult result;

      if(proc)
        result = proc(m_NBD, m_StructuredData);
      else
        SET_ERROR_RESULT(result, ResultCode::APIUnsupported,
                         "Can't get structured data for driver %s", m_NBD->GetDriverName().c_str());

      NoobDawn::Inst().SetProgressCallback<LoadProgress>(NOOBDAWN_ProgressCallback());

      return result;
    }

    RETURN_ERROR_RESULT(ResultCode::InvalidParameter,
                        "Can't initialise structured data for capture with no API data");
  }

  return RDResult();
}

nbdpair<ResultDetails, IReplayController *> CaptureFile::OpenCapture(const ReplayOptions &opts,
                                                                     NOOBDAWN_ProgressCallback progress)
{
  ResultDetails ret;
  ReplayController *render = NULL;

  if(!m_NBD)
    return {RDResult(ResultCode::InternalError, "NBD file unexpectedly NULL"), render};

  ret = m_NBD->Error();

  if(!ret.OK())
    return {ret, render};

  render = new ReplayController();

  LogReplayOptions(opts);

  NoobDawn::Inst().SetProgressCallback<LoadProgress>(progress);

  // This has to be before the device is created for the capture
  NoobDawn::Inst().ClearTrackedFiles();
  // This section is optional any errors do not block opening the capture
  if(m_NBD->SectionIndex(SectionType::EmbeddedExternalFiles) >= 0)
    ret = NoobDawn::Inst().ReadExternalFiles(m_NBD);

  ret = render->CreateDevice(m_NBD, opts);

  NoobDawn::Inst().SetProgressCallback<LoadProgress>(NOOBDAWN_ProgressCallback());

  if(!ret.OK())
  {
    render->Shutdown();
    render = NULL;
  }

  return {ret, render};
}

void CaptureFile::SetMetadata(const nbdstr &driverName, uint64_t machineIdent, FileType thumbType,
                              uint32_t thumbWidth, uint32_t thumbHeight, const bytebuf &thumbData,
                              uint64_t timeBase, double timeFreq)
{
  if(m_NBD)
  {
    NBDERR("Cannot set metadata on file that's already opened.");
    return;
  }

  NBDThumb *thumb = NULL;
  NBDThumb th;

  if(!thumbData.empty())
  {
    th = convertThumb(thumbType, thumbWidth, thumbHeight, thumbData);
    thumb = &th;
  }

  NBDDriver driver = driverFromName(driverName);

  if(driver == NBDDriver::Unknown)
  {
    NBDERR("Unrecognised driver name '%s'.", driverName.c_str());
    return;
  }

  m_NBD = new NBDFile;
  m_NBD->SetData(driver, driverName, machineIdent, thumb, timeBase, timeFreq);
}

ResultDetails CaptureFile::Convert(const nbdstr &filename, const nbdstr &filetype,
                                   const SDFile *file, NOOBDAWN_ProgressCallback progress)
{
  if(!m_NBD)
  {
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted,
                        "Data missing for creation of file, set metadata first.");
  }

  // make sure progress is valid so we don't have to check it everywhere
  if(!progress)
    progress = [](float) {};

  // we have two separate steps that can take time - fetching the structured data, and then
  // exporting or writing to NBD
  NOOBDAWN_ProgressCallback fetchProgress = [progress](float p) { progress(p * 0.5f); };
  NOOBDAWN_ProgressCallback exportProgress = [progress](float p) { progress(0.5f + p * 0.5f); };

  CaptureExporter exporter = NoobDawn::Inst().GetCaptureExporter(filetype);

  if(exporter)
  {
    if(file)
    {
      return exporter(filename, *m_NBD, *file, exportProgress);
    }
    else
    {
      RDResult result = InitStructuredData(fetchProgress);

      if(result != ResultCode::Succeeded)
        return result;

      return exporter(filename, *m_NBD, GetStructuredData(), exportProgress);
    }
  }

  if(filetype != "" && filetype != "nbd")
    NBDWARN("Converting file to unrecognised filetype '%s' - treating as 'nbd'", filetype.c_str());

  NBDFile output;

  output.SetData(m_NBD->GetDriver(), m_NBD->GetDriverName(), m_NBD->GetMachineIdent(),
                 &m_NBD->GetThumbnail(), m_NBD->GetTimestampBase(), m_NBD->GetTimestampFrequency());

  output.Create(filename);

  if(output.Error() != ResultCode::Succeeded)
    return output.Error();

  // when we don't have a frame capture section, write it from the structured data.
  int frameCaptureIndex = m_NBD->SectionIndex(SectionType::FrameCapture);

  if(frameCaptureIndex == -1)
  {
    RDResult result;
    if(file == NULL)
    {
      result = InitStructuredData(fetchProgress);
      file = &m_StructuredData;
    }

    if(result != ResultCode::Succeeded)
      return result;

    SectionProperties frameCapture;
    frameCapture.flags = SectionFlags::ZstdCompressed;
    frameCapture.type = SectionType::FrameCapture;
    frameCapture.name = ToStr(frameCapture.type);
    frameCapture.version = file->version;

    StreamWriter *writer = output.WriteSection(frameCapture);

    WriteSerialiser ser(writer, Ownership::Nothing);

    ser.WriteStructuredFile(*file, exportProgress);

    writer->Finish();

    RDResult ret = writer->GetError();

    delete writer;

    if(ret != ResultCode::Succeeded)
      return ret;
  }
  else
  {
    // otherwise write it straight, but compress it to zstd
    SectionProperties props = m_NBD->GetSectionProperties(frameCaptureIndex);
    props.flags = SectionFlags::ZstdCompressed;

    StreamWriter *writer = output.WriteSection(props);
    StreamReader *reader = m_NBD->ReadSection(frameCaptureIndex);

    StreamTransfer(writer, reader, progress);

    writer->Finish();

    RDResult ret = writer->GetError();
    if(ret == ResultCode::Succeeded)
      ret = reader->GetError();

    delete reader;
    delete writer;

    if(ret != ResultCode::Succeeded)
      return ret;
  }

  // write all other sections
  for(int i = 0; i < m_NBD->NumSections(); i++)
  {
    const SectionProperties &props = m_NBD->GetSectionProperties(i);

    if(props.type == SectionType::FrameCapture)
      continue;

    StreamWriter *writer = output.WriteSection(props);
    StreamReader *reader = m_NBD->ReadSection(i);

    StreamTransfer(writer, reader, NULL);

    writer->Finish();

    RDResult ret = writer->GetError();
    if(ret == ResultCode::Succeeded)
      ret = reader->GetError();

    delete reader;
    delete writer;

    if(ret != ResultCode::Succeeded)
      return ret;
  }

  return RDResult();
}

Thumbnail CaptureFile::GetThumbnail(FileType type, uint32_t maxsize)
{
  Thumbnail ret;
  ret.type = type;

  if(m_NBD == NULL)
    return ret;

  const NBDThumb &thumb = m_NBD->GetThumbnail();

  uint32_t thumbwidth = thumb.width, thumbheight = thumb.height;

  if(thumb.pixels.empty())
    return ret;

  bytebuf buf;

  // if the desired output is the format of stored thumbnail and either there's no max size or it's
  // already satisfied, return the data directly
  if(type == thumb.format && (maxsize == 0 || (maxsize > thumbwidth && maxsize > thumbheight)))
  {
    buf = thumb.pixels;
  }
  else
  {
    // otherwise we need to decode, resample maybe, and re-encode

    int w = (int)thumbwidth;
    int h = (int)thumbheight;
    int comp = 3;
    const byte *thumbpixels = NULL;
    byte *allocatedBuffer = NULL;
    switch(thumb.format)
    {
      case FileType::JPG:
        allocatedBuffer = jpgd::decompress_jpeg_image_from_memory(
            thumb.pixels.data(), (int)thumb.pixels.size(), &w, &h, &comp, 3);
        thumbpixels = allocatedBuffer;
        break;

      case FileType::Raw: thumbpixels = thumb.pixels.data(); break;

      default:
        allocatedBuffer =
            stbi_load_from_memory(thumb.pixels.data(), (int)thumb.pixels.size(), &w, &h, &comp, 3);
        if(allocatedBuffer == NULL)
        {
          NBDERR("Couldn't decode provided thumbnail");
          return ret;
        }
        thumbpixels = allocatedBuffer;
        break;
    }

    if(maxsize != 0)
    {
      uint32_t clampedWidth = NBDMIN(maxsize, thumbwidth);
      uint32_t clampedHeight = NBDMIN(maxsize, thumbheight);

      if(clampedWidth != thumbwidth || clampedHeight != thumbheight)
      {
        // preserve aspect ratio, take the smallest scale factor and multiply both
        float scaleX = float(clampedWidth) / float(thumbwidth);
        float scaleY = float(clampedHeight) / float(thumbheight);

        if(scaleX < scaleY)
          clampedHeight = uint32_t(scaleX * thumbheight);
        else if(scaleY < scaleX)
          clampedWidth = uint32_t(scaleY * thumbwidth);

        byte *resizedpixels = (byte *)malloc(3 * clampedWidth * clampedHeight);

        stbir_resize_uint8_srgb(thumbpixels, thumbwidth, thumbheight, 0, resizedpixels,
                                clampedWidth, clampedHeight, 0, STBIR_RGB);

        free(allocatedBuffer);

        allocatedBuffer = resizedpixels;
        thumbpixels = resizedpixels;
        thumbwidth = clampedWidth;
        thumbheight = clampedHeight;
      }
    }

    switch(type)
    {
      case FileType::Raw:
      {
        buf.assign(thumbpixels, thumbwidth * thumbheight * 3);
        break;
      }
      case FileType::JPG:
      {
        int len = thumbwidth * thumbheight * 3;
        buf.resize(len);
        jpge::params p;
        p.m_quality = 90;
        jpge::compress_image_to_jpeg_file_in_memory(buf.data(), len, (int)thumbwidth,
                                                    (int)thumbheight, 3, thumbpixels, p);
        buf.resize(len);
        break;
      }
      case FileType::PNG:
      {
        stbi_write_png_to_func(&writeToBytebuf, &buf, (int)thumbwidth, (int)thumbheight, 3,
                               thumbpixels, 0);
        break;
      }
      case FileType::TGA:
      {
        stbi_write_tga_to_func(&writeToBytebuf, &buf, (int)thumbwidth, (int)thumbheight, 3,
                               thumbpixels);
        break;
      }
      case FileType::BMP:
      {
        stbi_write_bmp_to_func(&writeToBytebuf, &buf, (int)thumbwidth, (int)thumbheight, 3,
                               thumbpixels);
        break;
      }
      default:
      {
        NBDERR("Unsupported file type %d in thumbnail fetch", type);
        free(allocatedBuffer);
        ret.width = 0;
        ret.height = 0;
        return ret;
      }
    }

    free(allocatedBuffer);
  }

  ret.data.swap(buf);
  ret.width = thumbwidth;
  ret.height = thumbheight;

  return ret;
}

int32_t CaptureFile::GetSectionCount()
{
  if(!m_NBD)
    return 0;

  return m_NBD->NumSections();
}

int32_t CaptureFile::FindSectionByName(const nbdstr &name)
{
  if(!m_NBD)
    return -1;

  return m_NBD->SectionIndex(name);
}

int32_t CaptureFile::FindSectionByType(SectionType type)
{
  if(!m_NBD)
    return -1;

  return m_NBD->SectionIndex(type);
}

SectionProperties CaptureFile::GetSectionProperties(int32_t index)
{
  if(!m_NBD || index < 0 || index >= m_NBD->NumSections())
    return SectionProperties();

  return m_NBD->GetSectionProperties(index);
}

bytebuf CaptureFile::GetSectionContents(int32_t index)
{
  bytebuf ret;

  if(!m_NBD || index < 0 || index >= m_NBD->NumSections())
    return ret;

  StreamReader *reader = m_NBD->ReadSection(index);

  ret.resize((size_t)reader->GetSize());
  bool success = reader->Read(ret.data(), reader->GetSize());

  delete reader;

  if(!success)
    ret.clear();

  return ret;
}

ResultDetails CaptureFile::WriteSection(const SectionProperties &props, const bytebuf &contents)
{
  if(!m_NBD)
  {
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted,
                        "Data missing for creation of file, set metadata first.");
  }

  RDResult nbdRes = m_NBD->Error();

  if(nbdRes != ResultCode::Succeeded)
    return nbdRes;

  StreamWriter *writer = m_NBD->WriteSection(props);
  nbdRes = m_NBD->Error();
  if(!writer || nbdRes != ResultCode::Succeeded)
    return nbdRes;

  writer->Write(contents.data(), contents.size());

  writer->Finish();

  delete writer;

  return RDResult();
}

bool CaptureFile::HasCallstacks()
{
  return m_NBD && m_NBD->SectionIndex(SectionType::ResolveDatabase) >= 0;
}

ResultDetails CaptureFile::InitResolver(bool interactive, NOOBDAWN_ProgressCallback progress)
{
  if(!m_NBD)
  {
    RETURN_ERROR_RESULT(ResultCode::FileCorrupted,
                        "Data missing for creation of file, set metadata first.");
  }

  if(!HasCallstacks())
  {
    RETURN_ERROR_RESULT(ResultCode::DataNotAvailable,
                        "Capture has no callstacks - can't initialise resolver.");
  }

  if(progress)
    progress(0.001f);

  int idx = m_NBD->SectionIndex(SectionType::ResolveDatabase);

  if(idx < 0)
  {
    RETURN_ERROR_RESULT(ResultCode::DataNotAvailable,
                        "Capture has no callstacks - can't initialise resolver.");
  }

  StreamReader *reader = m_NBD->ReadSection(idx);

  bytebuf buf;
  buf.resize((size_t)reader->GetSize());
  bool success = reader->Read(buf.data(), reader->GetSize());

  delete reader;

  if(!success)
  {
    RETURN_ERROR_RESULT(ResultCode::FileIOFailed, "Failed to read resolve database.");
  }

  if(progress)
    progress(0.002f);

  m_Resolver = Callstack::MakeResolver(interactive, buf.data(), buf.size(), progress);

  if(!m_Resolver)
  {
    RETURN_ERROR_RESULT(
        ResultCode::APIUnsupported,
        "Couldn't create callstack resolver - capture possibly from another platform.");
  }

  return RDResult();
}

nbdarray<nbdstr> CaptureFile::GetResolve(const nbdarray<uint64_t> &callstack)
{
  nbdarray<nbdstr> ret;

  if(callstack.empty())
    return ret;

  if(!m_Resolver)
  {
    ret = {""};
    return ret;
  }

  ret.reserve(callstack.size());
  for(uint64_t frame : callstack)
  {
    Callstack::AddressDetails info = m_Resolver->GetAddr(frame);
    ret.push_back(info.formattedString());
  }

  return ret;
}

ResultDetails CaptureFile::EmbedDependenciesIntoCapture()
{
  return NoobDawn::Inst().EmbedExternalFiles(m_NBD);
}

ResultDetails CaptureFile::RemoveDependenciesFromCapture()
{
  return NoobDawn::Inst().RemoveExternalFiles(m_NBD);
}

bool CaptureFile::HasEmbeddedDependencies()
{
  return NoobDawn::Inst().HasEmbeddedFiles(m_NBD);
}

bool CaptureFile::HasPendingDependencies()
{
  return NoobDawn::Inst().HasTrackedFileData();
}

nbdarray<nbdstr> CaptureFile::GetPendingDependenciesNicknames()
{
  return NoobDawn::Inst().GetTrackedFileNicknames();
}

extern "C" NOOBDAWN_API ICaptureFile *NOOBDAWN_CC NOOBDAWN_OpenCaptureFile()
{
  return new CaptureFile();
}
