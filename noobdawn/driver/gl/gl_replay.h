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

#pragma once

#include <unordered_map>
#include "api/replay/noobdawn_replay.h"
#include "core/core.h"
#include "replay/replay_driver.h"
#include "gl_common.h"

class AMDCounters;
class ARMCounters;
class IntelGlCounters;
class WrappedOpenGL;
struct GLCounterContext;
class NVGLCounters;

struct GLPostVSData
{
  struct InstData
  {
    uint32_t numVerts = 0;
    uint32_t bufOffset = 0;
  };

  struct StageData
  {
    GLuint buf = 0;
    Topology topo = Topology::Unknown;

    uint32_t vertStride = 0;

    // simple case - uniform
    uint32_t numVerts = 0;
    uint32_t instStride = 0;

    // complex case - expansion per instance
    nbdarray<InstData> instData;

    bool useIndices = false;
    GLuint idxBuf = 0;
    uint32_t idxByteWidth = 0;

    bool flipY = false;

    bool hasPosOut = false;

    float nearPlane = 0.0f;
    float farPlane = 0.0f;

    nbdstr status;
  } vsin, vsout, gsout;

  const StageData &GetStage(MeshDataStage type)
  {
    if(type == MeshDataStage::VSOut)
      return vsout;
    else if(type == MeshDataStage::GSOut)
      return gsout;

    if(type == MeshDataStage::Count)
    {
      if(gsout.buf)
        return gsout;

      return vsout;
    }

    NBDERR("Unexpected mesh data stage!");

    return vsin;
  }
};

struct CompleteCacheKey
{
  GLuint tex;
  GLuint samp;

  bool operator<(const CompleteCacheKey &o) const
  {
    if(tex != o.tex)
      return tex < o.tex;
    return samp < o.samp;
  }
};

enum TexDisplayFlags
{
  eTexDisplay_None = 0x0,
  eTexDisplay_BlendAlpha = 0x1,
  eTexDisplay_MipShift = 0x2,
  eTexDisplay_RemapFloat = 0x4,
  eTexDisplay_RemapUInt = 0x8,
  eTexDisplay_RemapSInt = 0x10,
};

struct SamplingProgramConfig
{
  enum
  {
    TexBuffer,
    Tex1D,
    Tex2D,
    Tex3D,
    TexCube,
    Tex1DArray,
    Tex2DArray,
    Tex3DArray,
    TexCubeArray,
    Tex2DRect,
    Tex2DMS,
    Tex2DMSArray,
  } dim = Tex2D;

  enum
  {
    Fetch,
    QueryLod,
    Sample,
    SampleDref,
    Gather,
    GatherDref,
  } op = Fetch;

  enum
  {
    Float,
    UInt,
    SInt,
  } resType = Float;

  uint32_t gatherChannel = 0;
  bool useGatherOffs = false;
  bool useGrad = false;
  bool manualBias = false;
  nbdfixedarray<int32_t, 8> gatherOffsets = {};
  Vec3i fetchOffset;

  uint32_t hashKey() const
  {
    uint32_t hash = 5381;
    hash = ((hash << 5) + hash) + (uint32_t)dim;
    hash = ((hash << 5) + hash) + (uint32_t)op;
    hash = ((hash << 5) + hash) + (uint32_t)resType;
    hash = ((hash << 5) + hash) + gatherChannel;
    hash = ((hash << 5) + hash) + useGatherOffs;
    hash = ((hash << 5) + hash) + useGrad;
    hash = ((hash << 5) + hash) + manualBias;
    hash = ((hash << 5) + hash) + fetchOffset.x;
    hash = ((hash << 5) + hash) + fetchOffset.y;
    hash = ((hash << 5) + hash) + fetchOffset.z;
    for(size_t i = 0; i < gatherOffsets.size(); i++)
      hash = ((hash << 5) + hash) + gatherOffsets[i];
    return hash;
  }
};

class GLReplay : public IReplayDriver
{
public:
  GLReplay(WrappedOpenGL *d);
  virtual ~GLReplay() {}
  void SetProxy(bool p) { m_Proxy = p; }
  bool IsRemoteProxy() { return m_Proxy; }
  void Shutdown();

  RDResult FatalErrorCheck();
  IReplayDriver *MakeDummyDriver();

  DriverInformation GetDriverInfo() { return m_DriverInfo; }
  nbdarray<GPUDevice> GetAvailableGPUs();
  APIProperties GetAPIProperties();

  ResourceDescription &GetResourceDesc(ResourceId id);
  nbdarray<ResourceDescription> GetResources();

  nbdarray<DescriptorStoreDescription> GetDescriptorStores() { return {}; }

  nbdarray<BufferDescription> GetBuffers();
  BufferDescription GetBuffer(ResourceId id);

  nbdarray<TextureDescription> GetTextures();
  TextureDescription GetTexture(ResourceId id);

  nbdarray<ShaderEntryPoint> GetShaderEntryPoints(ResourceId shader);
  const ShaderReflection *GetShader(ResourceId pipeline, ResourceId shader, ShaderEntryPoint entry);

  nbdarray<nbdstr> GetDisassemblyTargets(bool withPipeline);
  nbdstr DisassembleShader(ResourceId pipeline, const ShaderReflection *refl, const nbdstr &target);

  nbdarray<DebugMessage> GetDebugMessages();

  nbdarray<EventUsage> GetUsage(ResourceId id);

  FrameRecord &WriteFrameRecord() { return m_FrameRecord; }
  FrameRecord GetFrameRecord() { return m_FrameRecord; }
  void SetPipelineStates(D3D11Pipe::State *d3d11, D3D12Pipe::State *d3d12, GLPipe::State *gl,
                         VKPipe::State *vk)
  {
    m_GLPipelineState = gl;
  }
  void SavePipelineState(uint32_t eventId);
  nbdarray<Descriptor> GetDescriptors(ResourceId descriptorStore,
                                      const nbdarray<DescriptorRange> &ranges);
  nbdarray<SamplerDescriptor> GetSamplerDescriptors(ResourceId descriptorStore,
                                                    const nbdarray<DescriptorRange> &ranges);
  nbdarray<DescriptorAccess> GetDescriptorAccess(uint32_t eventId);
  nbdarray<DescriptorLogicalLocation> GetDescriptorLocations(ResourceId descriptorStore,
                                                             const nbdarray<DescriptorRange> &ranges);
  void FreeTargetResource(ResourceId id);

  RDResult ReadLogInitialisation(NBDFile *nbd, bool storeStructuredBuffers);
  void ReplayLog(uint32_t endEventID, ReplayLogType replayType);
  SDFile *GetStructuredFile();

  nbdarray<uint32_t> GetPassEvents(uint32_t eventId);

  nbdarray<WindowingSystem> GetSupportedWindowSystems();

  AMDRGPControl *GetRGPControl() { return NULL; }
  uint64_t MakeOutputWindow(WindowingData window, bool depth);
  void DestroyOutputWindow(uint64_t id);
  bool CheckResizeOutputWindow(uint64_t id);
  void GetOutputWindowDimensions(uint64_t id, int32_t &w, int32_t &h);
  void GetOutputWindowData(uint64_t id, bytebuf &retData);
  void ClearOutputWindowColor(uint64_t id, FloatVector col);
  void ClearOutputWindowDepth(uint64_t id, float depth, uint8_t stencil);
  void BindOutputWindow(uint64_t id, bool depth);
  bool IsOutputWindowVisible(uint64_t id);
  void FlipOutputWindow(uint64_t id);

  void InitPostVSBuffers(uint32_t eventId);
  void InitPostVSBuffers(const nbdarray<uint32_t> &passEvents);

  void PickPixel(ResourceId texture, uint32_t x, uint32_t y, const Subresource &sub,
                 CompType typeCast, float pixel[4]);
  bool GetMinMax(ResourceId texid, const Subresource &sub, CompType typeCast, float *minval,
                 float *maxval);
  bool GetHistogram(ResourceId texid, const Subresource &sub, CompType typeCast, float minval,
                    float maxval, const nbdfixedarray<bool, 4> &channels,
                    nbdarray<uint32_t> &histogram);

  MeshFormat GetPostVSBuffers(uint32_t eventId, uint32_t instID, uint32_t viewID,
                              MeshDataStage stage);

  void GetBufferData(ResourceId buff, uint64_t offset, uint64_t len, bytebuf &ret);
  void GetTextureData(ResourceId tex, const Subresource &sub, const GetTextureDataParams &params,
                      bytebuf &data);

  void ReplaceResource(ResourceId from, ResourceId to);
  void RemoveReplacement(ResourceId id);
  void ClearReplayCache();
  void ReloadShaderDebugInformation();

  nbdarray<GPUCounter> EnumerateCounters();
  CounterDescription DescribeCounter(GPUCounter counterID);
  nbdarray<CounterResult> FetchCounters(const nbdarray<GPUCounter> &counters);

  void RenderMesh(uint32_t eventId, const nbdarray<MeshFormat> &secondaryDraws,
                  const MeshDisplay &cfg);

  nbdarray<ShaderEncoding> GetCustomShaderEncodings() { return {ShaderEncoding::GLSL}; }
  nbdarray<ShaderSourcePrefix> GetCustomShaderSourcePrefixes()
  {
    // this is a complete hack, since we *do* want to define a prefix for GLSL. However GLSL sucks
    // and has the #version as the first thing, so we can't do a simple prepend of some defines.
    // Instead we will return no prefix and insert our own in BuildCustomShader if we see GLSL
    // coming in.
    return {};
  }
  nbdarray<ShaderEncoding> GetTargetShaderEncodings() { return {ShaderEncoding::GLSL}; }
  void BuildTargetShader(ShaderEncoding sourceEncoding, const bytebuf &source, const nbdstr &entry,
                         const ShaderCompileFlags &compileFlags, ShaderStage type, ResourceId &id,
                         nbdstr &errors);
  void SetCustomShaderIncludes(const nbdarray<nbdstr> &directories);
  void BuildCustomShader(ShaderEncoding sourceEncoding, const bytebuf &source, const nbdstr &entry,
                         const ShaderCompileFlags &compileFlags, ShaderStage type, ResourceId &id,
                         nbdstr &errors);
  void FreeCustomShader(ResourceId id);

  bool RenderTexture(TextureDisplay cfg);
  bool RenderTextureInternal(TextureDisplay cfg, TexDisplayFlags flags);

  void RenderCheckerboard(FloatVector dark, FloatVector light);

  void RenderHighlightBox(float w, float h, float scale);

  void FillCBufferVariables(ResourceId pipeline, ResourceId shader, ShaderStage stage,
                            nbdstr entryPoint, uint32_t cbufSlot, nbdarray<ShaderVariable> &outvars,
                            const bytebuf &data);

  nbdarray<PixelModification> PixelHistory(nbdarray<EventUsage> events, ResourceId target, uint32_t x,
                                           uint32_t y, const Subresource &sub, CompType typeCast);
  ShaderDebugTrace *DebugVertex(uint32_t eventId, uint32_t vertid, uint32_t instid, uint32_t idx,
                                uint32_t view);
  ShaderDebugTrace *DebugPixel(uint32_t eventId, uint32_t x, uint32_t y,
                               const DebugPixelInputs &inputs);
  ShaderDebugTrace *DebugThread(uint32_t eventId, const nbdfixedarray<uint32_t, 3> &groupid,
                                const nbdfixedarray<uint32_t, 3> &threadid);
  ShaderDebugTrace *DebugMeshThread(uint32_t eventId, const nbdfixedarray<uint32_t, 3> &groupid,
                                    const nbdfixedarray<uint32_t, 3> &threadid);
  nbdarray<ShaderDebugState> ContinueDebug(ShaderDebugger *debugger);
  void FreeDebugger(ShaderDebugger *debugger);
  uint32_t PickVertex(uint32_t eventId, int32_t width, int32_t height, const MeshDisplay &cfg,
                      uint32_t x, uint32_t y);

  ResourceId RenderOverlay(ResourceId texid, FloatVector clearCol, DebugOverlay overlay,
                           uint32_t eventId, const nbdarray<uint32_t> &passEvents);

  void BindFramebufferTexture(RenderOutputSubresource &sub, GLenum texBindingEnum, GLint numSamples);

  ResourceId ApplyCustomShader(TextureDisplay &display);

  ResourceId CreateProxyTexture(const TextureDescription &templateTex);
  void SetProxyTextureData(ResourceId texid, const Subresource &sub, byte *data, size_t dataSize);
  bool IsTextureSupported(const TextureDescription &tex);
  bool NeedRemapForFetch(const ResourceFormat &format);

  ResourceId CreateProxyBuffer(const BufferDescription &templateBuf);
  void SetProxyBufferData(ResourceId bufid, byte *data, size_t dataSize);

  RenderOutputSubresource GetRenderOutputSubresource(ResourceId id);
  bool IsRenderOutput(ResourceId id) { return GetRenderOutputSubresource(id).mip != ~0U; }
  void FileChanged() {}
  void SetReplayData(GLWindowingData data);

  void UseReplayContext() { MakeCurrentReplayContext(&m_ReplayCtx); }
  bool IsReplayContext(void *ctx) { return m_ReplayCtx.ctx == NULL || ctx == m_ReplayCtx.ctx; }
  bool HasDebugContext() { return m_DebugCtx != NULL; }
  void FillWithDiscardPattern(DiscardType type, GLuint framebuffer, GLsizei numAttachments,
                              const GLenum *attachments, GLint x, GLint y, GLsizei width,
                              GLsizei height);
  void FillWithDiscardPattern(DiscardType type, ResourceId id, GLuint mip, GLint xoffset = 0,
                              GLint yoffset = 0, GLint zoffset = 0, GLsizei width = 65536,
                              GLsizei height = 65536, GLsizei depth = 65536);

  bool CreateShaderReplacementProgram(GLuint srcProgram, GLuint srcPipeline, GLuint replacedProgram,
                                      ShaderStage stage, GLuint newShader, GLuint newShaderSPIRV);

  GLuint GetShaderDebugMathProg();
  GLuint MakeShaderDebugSampleProg(const SamplingProgramConfig &config);

private:
  void OpenGLFillCBufferVariables(ResourceId shader, GLuint prog, bool bufferBacked, nbdstr prefix,
                                  const nbdarray<ShaderConstant> &variables,
                                  nbdarray<ShaderVariable> &outvars, const bytebuf &data);

  bool GetMinMax(ResourceId texid, const Subresource &sub, CompType typeCast, bool stencil,
                 float *minval, float *maxval);

  void CreateCustomShaderTex(uint32_t w, uint32_t h);

  struct OutputWindow : public GLWindowingData
  {
    OutputWindow(const GLWindowingData &data) : GLWindowingData(data) {}
    OutputWindow() {}
    struct
    {
      // used to blit from defined FBO (VAOs not shared)
      GLuint emptyVAO = 0;

      // textures for the below FBO. Resize with the window
      GLuint backbuffer = 0;
      GLuint depthstencil = 0;

      // this FBO is on the debug GL context, not the window's GL context
      // when rendering a texture or mesh etc, we render onto this FBO on
      // the debug GL context, then blit onto the default framebuffer
      // on the window's GL context.
      // This is so we don't have to re-create any non-shared resource we
      // need for debug rendering on the window's GL context.
      GLuint windowFBO = 0;

      // this FBO is the same as the above, but on the replay context,
      // for any cases where we need to use the replay context (like
      // re-rendering an action).
      GLuint replayFBO = 0;

      // read FBO for blit to window
      GLuint readFBO = 0;
    } BlitData;

    WindowingSystem system = WindowingSystem::Headless;
    int width = 1, height = 1;
  };

  enum class TextureSamplerMode
  {
    Point,
    PointNoMip,
    Linear,
  };

  struct TextureSamplerState
  {
    GLenum minFilter = eGL_NEAREST;
    GLenum magFilter = eGL_NEAREST;
    GLenum wrapS = eGL_CLAMP_TO_EDGE;
    GLenum wrapT = eGL_CLAMP_TO_EDGE;
    GLenum wrapR = eGL_CLAMP_TO_EDGE;
    GLenum compareMode = eGL_NONE;
    float lodBias = 0.0f;
  };

  // sets the desired parameters, and returns the previous ones ready to restore
  TextureSamplerState SetSamplerParams(GLenum target, GLuint texname, TextureSamplerMode mode);
  void RestoreSamplerParams(GLenum target, GLuint texname, TextureSamplerState state);

  // any objects that are shared between contexts, we just initialise
  // once
  struct DebugRenderData
  {
    float outWidth, outHeight;

    int glslVersion;

    nbdstr texSampleDefines;

    // min/max data
    GLuint minmaxTileResult;          // tile result buffer
    GLuint minmaxResult;              // Vec4f[2] final result buffer
    GLuint histogramBuf;              // uint32_t * num buckets buffer
    GLuint minmaxResultProgram[3];    // float/uint/sint tile result -> final result program
    GLuint minmaxTileProgram[64];     // RESTYPE indexed (see glsl_ubos.h, 1d/2d/3d etc |
                                      // uint/sint) src tex -> tile result buf program
    GLuint histogramProgram[64];      // RESTYPE indexed (see glsl_ubos.h, 1d/2d/3d etc |
                                      // uint/sint) src tex -> histogram result buf program

    GLuint texDisplayVertexShader;
    GLuint texDisplayProg[3];    // float/uint/sint
    GLuint texRemapProg[3];      // float/uint/sint

    GLuint customFBO;
    GLuint customTex;
    ResourceId CustomShaderTexID;

    static const int maxMeshPicks = 500;

    GLuint meshPickProgram;
    GLuint pickIBBuf, pickVBBuf;
    uint32_t pickIBSize, pickVBSize;
    GLuint pickResultBuf;

    GLuint checkerProg;

    GLuint fixedcolFragShader;
    GLuint fixedcolFragShaderSPIRV;

    // 0 = both floats, 1 = position doubles, 2 = secondary doubles, 3 = both doubles
    GLuint meshProg[4];
    GLuint meshgsProg[4];
    GLuint trisizeProg;

    GLuint meshVAO;
    GLuint axisVAO;
    GLuint frustumVAO;
    GLuint triHighlightVAO;

    GLuint axisFrustumBuffer;
    GLuint triHighlightBuffer;

    GLuint feedbackObj;
    nbdarray<GLuint> feedbackQueries;
    GLuint feedbackBuffer;
    uint64_t feedbackBufferSize = 32 * 1024 * 1024;

    GLuint pickPixelTex;
    GLuint pickPixelFBO;

    GLuint dummyTexBuffer;
    GLuint dummyTexBufferStore;

    GLuint quadoverdrawFragShader;
    GLuint quadoverdrawFragShaderSPIRV;
    GLuint quadoverdrawResolveProg;

    GLuint fullScreenFixedColProg;
    GLuint fullScreenCopyDepth;
    GLuint fullScreenCopyDepthMS;

    GLuint discardProg[3][4];
    GLuint discardPatternBuffer;

    GLuint shaderDebugMathProg;

    ResourceId overlayTexId;
    GLuint overlayTex;
    GLuint overlayFBO;
    GLuint overlayProg;
    GLint overlayTexWidth = 0, overlayTexHeight = 0, overlayTexSamples = 0, overlayTexMips = 0,
          overlayTexSlices = 0;

    GLuint UBOs[3];

    GLuint emptyVAO;
  } DebugData;

  bool m_Degraded;

  HighlightCache m_HighlightCache;

  std::unordered_map<uint32_t, GLuint> m_ShaderDebugSampleProg;

  std::map<GLenum, bytebuf> m_DiscardPatterns;

  // eventId -> data
  std::map<uint32_t, GLPostVSData> m_PostVSData;

  void ClearPostVSCache();

  // cache the previous data returned
  ResourceId m_GetTexturePrevID;
  byte *m_GetTexturePrevData[16];

  GLuint CreateMeshProgram(GLuint vs, GLuint fs, GLuint gs = 0);
  void ConfigureTexDisplayProgramBindings(GLuint program);
  void BindUBO(GLuint program, const char *name, GLuint binding);

  void InitDebugData();
  void DeleteDebugData();

  void CheckGLSLVersion(const char *sl, int &glslVersion);

  void FillTimers(GLCounterContext &ctx, const ActionDescription &actionnode,
                  const nbdarray<GPUCounter> &counters);

  void InitOutputWindow(OutputWindow &outwin);
  void CreateOutputWindowBackbuffer(OutputWindow &outwin, bool depth);

  GLWindowingData m_ReplayCtx;
  uint64_t m_DebugID;
  OutputWindow *m_DebugCtx;

  void MakeCurrentReplayContext(GLWindowingData *ctx);
  void SwapBuffers(GLWindowingData *ctx);
  void CloseReplayContext();

  uint64_t m_OutputWindowID;
  std::map<uint64_t, OutputWindow> m_OutputWindows;

  bool m_Proxy;

  void CacheTexture(ResourceId id);

  std::map<ResourceId, TextureDescription> m_CachedTextures;

  WrappedOpenGL *m_pDriver;

  nbdarray<ResourceDescription> m_Resources;
  std::map<ResourceId, size_t> m_ResourceIdx;

  GLPipe::State *m_GLPipelineState = NULL;

  nbdarray<DescriptorAccess> m_Access;

  FrameRecord m_FrameRecord;

  DriverInformation m_DriverInfo;

  std::map<CompleteCacheKey, nbdstr> m_CompleteCache;

  // AMD counter instance
  AMDCounters *m_pAMDCounters = NULL;

  void FillTimersAMD(uint32_t *eventStartID, uint32_t *sampleIndex, nbdarray<uint32_t> *eventIDs,
                     const ActionDescription &actionnode);

  nbdarray<CounterResult> FetchCountersAMD(const nbdarray<GPUCounter> &counters);

  // Intel counter instance
  IntelGlCounters *m_pIntelCounters = NULL;

  void FillTimersIntel(uint32_t *eventStartID, uint32_t *sampleIndex, nbdarray<uint32_t> *eventIDs,
                       const ActionDescription &actionnode);

  nbdarray<CounterResult> FetchCountersIntel(const nbdarray<GPUCounter> &counters);

  // ARM counter instance
  ARMCounters *m_pARMCounters = NULL;

  void FillTimersARM(uint32_t *eventStartID, uint32_t *sampleIndex, nbdarray<uint32_t> *eventIDs,
                     const ActionDescription &actionnode);

  nbdarray<CounterResult> FetchCountersARM(const nbdarray<GPUCounter> &counters);

#if DISABLED(RDOC_ANDROID) && DISABLED(RDOC_APPLE)
  // NVIDIA counter instance
  NVGLCounters *m_pNVCounters = NULL;
#endif
};
