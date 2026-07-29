/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2020-2026 Baldur Karlsson
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

#include "core/settings.h"
#include "driver/shaders/spirv/spirv_debug.h"
#include "driver/shaders/spirv/spirv_editor.h"
#include "driver/shaders/spirv/spirv_op_helpers.h"
#include "maths/formatpacking.h"
#include "replay/common/var_dispatch_helpers.h"
#include "vk_core.h"
#include "vk_debug.h"
#include "vk_replay.h"
#include "vk_shader_cache.h"

#undef None

RDOC_CONFIG(nbdstr, Vulkan_Debug_PSDebugDumpDirPath, "",
            "Path to dump shader debugging generated SPIR-V files.");
RDOC_CONFIG(bool, Vulkan_Debug_ShaderDebugLogging, false,
            "Output verbose debug logging messages when debugging shaders.");

RDOC_CONFIG(bool, Vulkan_Debug_EnableShaderDebugMT, true,
            "Use multiple threads to run the shader debugger simulation.");

// needed for old linux compilers
namespace std
{
template <>
struct hash<ShaderBuiltin>
{
  std::size_t operator()(const ShaderBuiltin &e) const { return size_t(e); }
};
}

// should match the descriptor set layout created in ShaderDebugData::Init()
enum class ShaderDebugBind
{
  Tex1D = 1,
  First = Tex1D,
  Tex2D = 2,
  Tex3D = 3,
  Tex2DMS = 4,
  TexCube = 5,
  Buffer = 6,
  Sampler = 7,
  Constants = 8,
  Count,
  MathResult = 9,
};

struct GatherOffsets
{
  int32_t u0, v0, u1, v1, u2, v2, u3, v3;
};

struct ShaderConstParameters
{
  uint32_t operation;
  VkBool32 useGradOrGatherOffsets;
  ShaderDebugBind dim;
  nbdspv::GatherChannel gatherChannel;
  union
  {
    GatherOffsets gatherOffsets;
    Vec3i constOffsets;
  };

  uint32_t hashKey(uint32_t shaderIndex) const
  {
    uint32_t hash = 5381;
    hash = ((hash << 5) + hash) + shaderIndex;
    hash = ((hash << 5) + hash) + operation;
    hash = ((hash << 5) + hash) + useGradOrGatherOffsets;
    hash = ((hash << 5) + hash) + (uint32_t)dim;
    hash = ((hash << 5) + hash) + (uint32_t)gatherChannel;
    hash = ((hash << 5) + hash) + gatherOffsets.u0;
    hash = ((hash << 5) + hash) + gatherOffsets.v0;
    hash = ((hash << 5) + hash) + gatherOffsets.u1;
    hash = ((hash << 5) + hash) + gatherOffsets.v1;
    hash = ((hash << 5) + hash) + gatherOffsets.u2;
    hash = ((hash << 5) + hash) + gatherOffsets.v2;
    hash = ((hash << 5) + hash) + gatherOffsets.u3;
    hash = ((hash << 5) + hash) + gatherOffsets.v3;
    return hash;
  }
};

struct ShaderUniformParameters
{
  Vec3i texel_uvw;
  int texel_lod;
  float uvwa[4];
  float ddx[3];
  float ddy[3];
  Vec3i offset;
  int sampleIdx;
  float compare;
  float lod;
  float minlod;
};

#if ENABLED(RDOC_RELEASE)
#define CHECK_DEVICE_THREAD()
#else
#define CHECK_DEVICE_THREAD() \
  NBDASSERTMSG("API Wrapper function called from non-device thread!", IsDeviceThread());
#endif    // #if ENABLED(RDOC_RELEASE)

class VulkanAPIWrapper : public nbdspv::DebugAPIWrapper
{
public:
  VulkanAPIWrapper(WrappedVulkan *vk, VulkanCreationInfo &creation, ShaderStage stage, uint32_t eid,
                   ResourceId shadId)
      : m_DebugData(vk->GetReplay()->GetShaderDebugData()),
        m_Creation(creation),
        m_EventID(eid),
        m_ShaderID(shadId),
        deviceThreadID(Threading::GetCurrentID())
  {
    m_pDriver = vk;

    // when we're first setting up, the state is pristine and no replay is needed
    m_ResourcesDirty = false;

    VulkanReplay *replay = m_pDriver->GetReplay();

    // cache the descriptor access. This should be a superset of all descriptors we need to read from
    m_Access = replay->GetDescriptorAccess(eid);

    // filter to only accesses from the stage we care about, as access lookups will be stage-specific
    m_Access.removeIf([stage](const DescriptorAccess &access) { return access.stage != stage; });

    // fetch all descriptor contents now too
    m_Descriptors.reserve(m_Access.size());
    m_SamplerDescriptors.reserve(m_Access.size());

    // we could collate ranges by descriptor store, but in practice we don't expect descriptors to
    // be scattered across multiple stores. So to keep the code simple for now we do a linear sweep
    ResourceId store;
    nbdarray<DescriptorRange> ranges;

    for(const DescriptorAccess &acc : m_Access)
    {
      if(acc.descriptorStore != store)
      {
        if(store != ResourceId())
        {
          m_Descriptors.append(replay->GetDescriptors(store, ranges));
          m_SamplerDescriptors.append(replay->GetSamplerDescriptors(store, ranges));
        }

        store = acc.descriptorStore;
        ranges.clear();
      }

      // if the last range is contiguous with this access, append this access as a new range to query
      if(!ranges.empty() && ranges.back().descriptorSize == acc.byteSize &&
         ranges.back().offset + ranges.back().descriptorSize == acc.byteOffset &&
         ranges.back().type == acc.type)
      {
        ranges.back().count++;
        continue;
      }

      DescriptorRange range = acc;
      ranges.push_back(range);
    }

    if(store != ResourceId())
    {
      m_Descriptors.append(replay->GetDescriptors(store, ranges));
      m_SamplerDescriptors.append(replay->GetSamplerDescriptors(store, ranges));
    }

    // apply dynamic offsets to our cached descriptors
    // we iterate over descriptors first to find dynamic ones, then iterate over our cached set to
    // apply. Neither array should be large but there should be fewer dynamic descriptors in total
    {
      const VulkanRenderState &state = m_pDriver->GetRenderState();

      const nbdarray<VulkanStatePipeline::DescriptorAndOffsets> &src =
          (stage == ShaderStage::Compute ? state.compute.descSets : state.graphics.descSets);

      for(size_t i = 0; i < src.size(); i++)
      {
        const VulkanStatePipeline::DescriptorAndOffsets &srcData = src[i];
        ResourceId sourceSet = srcData.descSet;
        const uint32_t *srcOffset = srcData.offsets.begin();

        // this could be either an unbound set, or descriptor buffers (which can't use dynamic offsets anyway)
        if(sourceSet == ResourceId())
          continue;

        const VulkanCreationInfo::PipelineLayout &pipeLayoutInfo =
            m_Creation.GetPipelineLayoutInfo(srcData.pipeLayout);

        ResourceId setOrig = sourceSet;

        const BindingStorage &bindStorage =
            m_pDriver->GetCurrentDescSetBindingStorage(srcData.descSet);
        const DescriptorSetSlot *first = bindStorage.binds.empty() ? NULL : bindStorage.binds[0];
        for(size_t b = 0; b < bindStorage.binds.size(); b++)
        {
          const DescSetLayout::Binding &layoutBind =
              m_Creation.GetDescSetLayout(pipeLayoutInfo.descSetLayouts[i]).bindings[b];

          if(layoutBind.layoutDescType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
             layoutBind.layoutDescType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
            continue;

          uint64_t descriptorByteOffset = bindStorage.binds[b] - first;

          // inline UBOs aren't dynamic and variable size can't be used with dynamic buffers, so
          // the count is what it is at definition time
          for(uint32_t a = 0; a < layoutBind.descriptorCount; a++)
          {
            uint32_t dynamicBufferByteOffset = *srcOffset;
            srcOffset++;

            for(size_t accIdx = 0; accIdx < m_Access.size(); accIdx++)
            {
              if(m_Access[accIdx].descriptorStore == setOrig &&
                 m_Access[accIdx].byteOffset == descriptorByteOffset + a)
              {
                m_Descriptors[accIdx].byteOffset += dynamicBufferByteOffset;
                break;
              }
            }
          }
        }
      }
    }
    NBDASSERT(ShaderDebugData::MAX_QUEUED_OPS * mathOpResultByteSize <
              m_DebugData.ReadbackBuffer.TotalSize());
    NBDASSERT(sampleGatherOpResultsStart +
                  ShaderDebugData::MAX_QUEUED_OPS * sampleGatherOpResultByteSize <
              m_DebugData.ReadbackBuffer.TotalSize());
  }

  ~VulkanAPIWrapper()
  {
    CHECK_DEVICE_THREAD();
    m_pDriver->FlushQ();

    VkDevice dev = m_pDriver->GetDev();
    for(auto it = m_SampleViews.begin(); it != m_SampleViews.end(); it++)
      m_pDriver->vkDestroyImageView(dev, it->second, NULL);
    for(auto it = m_BiasSamplers.begin(); it != m_BiasSamplers.end(); it++)
      m_pDriver->vkDestroySampler(dev, it->second, NULL);
  }

  void ResetReplay()
  {
    CHECK_DEVICE_THREAD();
    if(!m_ResourcesDirty)
    {
      VkMarkerRegion region("ResetReplay");
      // replay the action to get back to 'normal' state for this event, and mark that we need to
      // replay back to pristine state next time we need to fetch data.
      m_pDriver->ReplayLog(0, m_EventID, eReplay_OnlyDraw);
    }
    m_ResourcesDirty = true;
  }

  virtual void AddDebugMessage(MessageCategory cat, MessageSeverity sev, MessageSource src,
                               nbdstr desc) override
  {
    CHECK_DEVICE_THREAD();
    m_pDriver->AddDebugMessage(cat, sev, src, desc);
  }

  virtual GraphicsAPI GetGraphicsAPI() override { return GraphicsAPI::Vulkan; }

  virtual bool SimulateThreaded() override { return Vulkan_Debug_EnableShaderDebugMT(); }

  virtual ResourceId GetShaderID() override { return m_ShaderID; }

  virtual uint64_t GetBufferLength(const ShaderBindIndex &bind) override
  {
    nbdspv::DeviceOpResult opResult;
    size_t length = 0;
    // BufferFunction guarantees the buffer cache readlock whilst the function is called
    bool succeeded = BufferFunction(
        bind, [&length](bytebuf *data) { length = data->size(); }, opResult);
    NBDASSERT(succeeded);
    NBDASSERTEQUAL(opResult, nbdspv::DeviceOpResult::Succeeded);
    return length;
  }

  virtual void ReadLocationValue(int32_t location, ShaderVariable &var) override
  {
    NBDERR("Invalid by-location read");
  }

  virtual void ReadBufferValue(const ShaderBindIndex &bind, uint64_t offset, uint64_t byteSize,
                               void *dst) override
  {
    nbdspv::DeviceOpResult opResult;
    // BufferFunction guarantees the buffer cache readlock whilst the function is called
    bool succeeded = BufferFunction(
        bind,
        [offset, byteSize, dst](bytebuf *data) {
          if(offset + byteSize <= data->size())
            memcpy(dst, data->data() + (size_t)offset, (size_t)byteSize);
        },
        opResult);
    NBDASSERT(succeeded);
    NBDASSERTEQUAL(opResult, nbdspv::DeviceOpResult::Succeeded);
  }

  virtual void WriteBufferValue(const ShaderBindIndex &bind, uint64_t offset, uint64_t byteSize,
                                const void *src) override
  {
    nbdspv::DeviceOpResult opResult;
    // BufferFunction guarantees the buffer cache readlock whilst the function is called
    bool succeeded = BufferFunction(
        bind,
        [offset, byteSize, src](bytebuf *data) {
          if(offset + byteSize <= data->size())
            memcpy(data->data() + (size_t)offset, src, (size_t)byteSize);
        },
        opResult);
    NBDASSERT(succeeded);
    NBDASSERTEQUAL(opResult, nbdspv::DeviceOpResult::Succeeded);
  }

  virtual void ReadAddress(uint64_t address, uint64_t byteSize, void *dst) override
  {
    nbdspv::DeviceOpResult opResult;
    // BufferFunction guarantees the buffer cache readlock whilst the function is called
    bool succeeded = BufferFunction(
        address,
        [byteSize, dst](bytebuf *data, size_t offset) {
          if(offset + byteSize <= data->size())
            memcpy(dst, data->data() + offset, (size_t)byteSize);
        },
        opResult);
    NBDASSERT(succeeded);
    NBDASSERTEQUAL(opResult, nbdspv::DeviceOpResult::Succeeded);
  }

  virtual void WriteAddress(uint64_t address, uint64_t byteSize, const void *src) override
  {
    nbdspv::DeviceOpResult opResult;
    // BufferFunction guarantees the buffer cache readlock whilst the function is called
    bool succeeded = BufferFunction(
        address,
        [byteSize, src](bytebuf *data, size_t offset) {
          if(offset + byteSize <= data->size())
            memcpy(data->data() + offset, src, (size_t)byteSize);
        },
        opResult);
    NBDASSERT(succeeded);
    NBDASSERTEQUAL(opResult, nbdspv::DeviceOpResult::Succeeded);
  }

  // Called from any thread
  // Caller guarantees that if the image data is not cached then we are on the device thread
  virtual nbdspv::DeviceOpResult ReadTexel(const ShaderBindIndex &imageBind,
                                           const ShaderVariable &coord, uint32_t sample,
                                           ShaderVariable &output) override
  {
    nbdspv::DeviceOpResult opResult;
    bool isCached = false;
    {
      SCOPED_READLOCK(imageCacheLock);
      isCached = GetImageDataFromCache(imageBind, opResult) != NULL;
      NBDASSERTNOTEQUAL(opResult, nbdspv::DeviceOpResult::NeedsDevice);
    }

    if(!isCached)
    {
      // Add image data to the cache : cache should not be locked by this thread
      PopulateImage(imageBind);
    }

    {
      SCOPED_READLOCK(imageCacheLock);
      ImageData *result = GetImageDataFromCache(imageBind, opResult);
      if(!result)
      {
        NBDASSERTEQUAL(opResult, nbdspv::DeviceOpResult::Failed);
        return nbdspv::DeviceOpResult::Failed;
      }

      ImageData &data = *result;
      if(data.width == 0)
        return nbdspv::DeviceOpResult::Failed;

      uint32_t coords[4];
      for(int i = 0; i < 4; i++)
        coords[i] = uintComp(coord, i);

      if(coords[0] >= data.width || coords[1] >= data.height || coords[2] >= data.depth)
      {
        if(!IsDeviceThread())
          return nbdspv::DeviceOpResult::NeedsDevice;

        CHECK_DEVICE_THREAD();
        m_pDriver->AddDebugMessage(
            MessageCategory::Execution, MessageSeverity::High, MessageSource::RuntimeWarning,
            StringFormat::Fmt(
                "Out of bounds access to image, coord %u,%u,%u outside of dimensions %ux%ux%u",
                coords[0], coords[1], coords[2], data.width, data.height, data.depth));
        return nbdspv::DeviceOpResult::Failed;
      }

      CompType varComp = VarTypeCompType(output.type);

      set0001(output);

      ShaderVariable input;
      input.columns = data.fmt.compCount;

      // the only 'irregular' format we need to worry about handling for integer types is
      // 10:10:10:2. All others are float/uint
      if(data.fmt.type == ResourceFormatType::R10G10B10A2)
      {
        PixelValue val;
        DecodePixelData(data.fmt, data.texel(coords, sample), val);

        if(data.fmt.compType == CompType::UInt)
          input.type = VarType::UInt;
        else if(data.fmt.compType == CompType::SInt)
          input.type = VarType::SInt;
        else
          input.type = VarType::Float;

        memcpy(input.value.u32v.data(), val.uintValue.data(), val.uintValue.byteSize());

        for(uint8_t c = 0; c < NBDMIN(output.columns, input.columns); c++)
        {
          if(data.fmt.compType == CompType::UInt)
            setUintComp(output, c, uintComp(input, c));
          else if(data.fmt.compType == CompType::SInt)
            setIntComp(output, c, intComp(input, c));
          else
            setFloatComp(output, c, input.value.f32v[c]);
        }
      }
      else if(data.fmt.compType == CompType::UInt)
      {
        NBDASSERT(varComp == CompType::UInt, varComp);

        // set up input type for proper expansion below
        if(data.fmt.compByteWidth == 1)
          input.type = VarType::UByte;
        else if(data.fmt.compByteWidth == 2)
          input.type = VarType::UShort;
        else if(data.fmt.compByteWidth == 4)
          input.type = VarType::UInt;
        else if(data.fmt.compByteWidth == 8)
          input.type = VarType::ULong;

        memcpy(input.value.u8v.data(), data.texel(coords, sample), data.texelSize);

        for(uint8_t c = 0; c < NBDMIN(output.columns, input.columns); c++)
          setUintComp(output, c, uintComp(input, c));
      }
      else if(data.fmt.compType == CompType::SInt)
      {
        NBDASSERT(varComp == CompType::SInt, varComp);

        // set up input type for proper expansion below
        if(data.fmt.compByteWidth == 1)
          input.type = VarType::SByte;
        else if(data.fmt.compByteWidth == 2)
          input.type = VarType::SShort;
        else if(data.fmt.compByteWidth == 4)
          input.type = VarType::SInt;
        else if(data.fmt.compByteWidth == 8)
          input.type = VarType::SLong;

        memcpy(input.value.u8v.data(), data.texel(coords, sample), data.texelSize);

        for(uint8_t c = 0; c < NBDMIN(output.columns, input.columns); c++)
          setIntComp(output, c, intComp(input, c));
      }
      else
      {
        NBDASSERT(varComp == CompType::Float, varComp);

        // do the decode of whatever unorm/float/etc the format is
        FloatVector v = DecodeFormattedComponents(data.fmt, data.texel(coords, sample));

        // set it into f32v
        input.value.f32v[0] = v.x;
        input.value.f32v[1] = v.y;
        input.value.f32v[2] = v.z;
        input.value.f32v[3] = v.w;

        // read as floats
        input.type = VarType::Float;

        for(uint8_t c = 0; c < NBDMIN(output.columns, input.columns); c++)
          setFloatComp(output, c, input.value.f32v[c]);
      }
    }

    return nbdspv::DeviceOpResult::Succeeded;
  }

  // Called from any thread
  // Caller guarantees that if the image data is not cached then we are on the device thread
  virtual nbdspv::DeviceOpResult WriteTexel(const ShaderBindIndex &imageBind,
                                            const ShaderVariable &coord, uint32_t sample,
                                            const ShaderVariable &input) override
  {
    nbdspv::DeviceOpResult opResult;
    ImageData *result = NULL;
    {
      SCOPED_READLOCK(imageCacheLock);
      result = GetImageDataFromCache(imageBind, opResult);
      NBDASSERTNOTEQUAL(opResult, nbdspv::DeviceOpResult::NeedsDevice);
    }

    if(!result)
    {
      // Add image data to the cache : cache should not be locked by this thread
      PopulateImage(imageBind);
    }

    {
      SCOPED_READLOCK(imageCacheLock);
      result = GetImageDataFromCache(imageBind, opResult);
      if(!result)
        return nbdspv::DeviceOpResult::Failed;

      ImageData &data = *result;
      if(data.width == 0)
        return nbdspv::DeviceOpResult::Failed;

      uint32_t coords[4];
      for(int i = 0; i < 4; i++)
        coords[i] = uintComp(coord, i);

      if(coords[0] >= data.width || coords[1] >= data.height || coords[2] >= data.depth)
      {
        if(!IsDeviceThread())
          return nbdspv::DeviceOpResult::NeedsDevice;

        CHECK_DEVICE_THREAD();
        m_pDriver->AddDebugMessage(
            MessageCategory::Execution, MessageSeverity::High, MessageSource::RuntimeWarning,
            StringFormat::Fmt(
                "Out of bounds access to image, coord %u,%u,%u outside of dimensions %ux%ux%u",
                coords[0], coords[1], coords[2], data.width, data.height, data.depth));
        return nbdspv::DeviceOpResult::Failed;
      }

      CompType varComp = VarTypeCompType(input.type);

      ShaderVariable output;
      output.columns = data.fmt.compCount;

      // the only 'irregular' format we need to worry about handling for integer types is
      // 10:10:10:2. All others are float/uint
      if(data.fmt.type == ResourceFormatType::R10G10B10A2)
      {
        // image writes are required to write a whole texel so we know we should have 4 components
        NBDASSERTEQUAL(input.columns, 4);

        uint32_t encoded = 0;

        if(data.fmt.compType == CompType::SNorm)
          encoded = ConvertToR10G10B10A2SNorm(Vec4f(input.value.f32v[0], input.value.f32v[1],
                                                    input.value.f32v[2], input.value.f32v[3]));
        else if(data.fmt.compType == CompType::UInt)
          encoded = ConvertToR10G10B10A2(Vec4u(input.value.u32v[0], input.value.u32v[1],
                                               input.value.u32v[2], input.value.u32v[3]));
        else
          encoded = ConvertToR10G10B10A2(Vec4f(input.value.f32v[0], input.value.f32v[1],
                                               input.value.f32v[2], input.value.f32v[3]));

        memcpy(data.texel(coords, sample), &encoded, sizeof(uint32_t));
      }
      else if(data.fmt.compType == CompType::UInt)
      {
        NBDASSERT(varComp == CompType::UInt, varComp);

        // set up output type for proper expansion below
        if(data.fmt.compByteWidth == 1)
          output.type = VarType::UByte;
        else if(data.fmt.compByteWidth == 2)
          output.type = VarType::UShort;
        else if(data.fmt.compByteWidth == 4)
          output.type = VarType::UInt;
        else if(data.fmt.compByteWidth == 8)
          output.type = VarType::ULong;

        for(uint8_t c = 0; c < NBDMIN(output.columns, input.columns); c++)
          setUintComp(output, c, uintComp(input, c));

        memcpy(data.texel(coords, sample), output.value.u8v.data(), data.texelSize);
      }
      else if(data.fmt.compType == CompType::SInt)
      {
        NBDASSERT(varComp == CompType::SInt, varComp);

        // set up input type for proper expansion below
        if(data.fmt.compByteWidth == 1)
          output.type = VarType::SByte;
        else if(data.fmt.compByteWidth == 2)
          output.type = VarType::SShort;
        else if(data.fmt.compByteWidth == 4)
          output.type = VarType::SInt;
        else if(data.fmt.compByteWidth == 8)
          output.type = VarType::SLong;

        for(uint8_t c = 0; c < NBDMIN(output.columns, input.columns); c++)
          setIntComp(output, c, intComp(input, c));

        memcpy(data.texel(coords, sample), output.value.u8v.data(), data.texelSize);
      }
      else
      {
        NBDASSERT(varComp == CompType::Float, varComp);

        // read as floats
        output.type = VarType::Float;

        for(uint8_t c = 0; c < NBDMIN(output.columns, input.columns); c++)
          setFloatComp(output, c, input.value.f32v[c]);

        FloatVector v;

        // set it into f32v
        v.x = input.value.f32v[0];
        v.y = input.value.f32v[1];
        v.z = input.value.f32v[2];
        v.w = input.value.f32v[3];

        EncodeFormattedComponents(data.fmt, v, data.texel(coords, sample));
      }
    }
    return nbdspv::DeviceOpResult::Succeeded;
  }

  // Can be called from any thread
  virtual void FillInputValue(ShaderVariable &var, ShaderBuiltin builtin, uint32_t threadIndex,
                              uint32_t location, uint32_t component) const override
  {
    if(!inputVarsReadOnly)
    {
      NBDERR("Input variables still being filled in");
      return;
    }
    if(builtin != ShaderBuiltin::Undefined)
    {
      if(threadIndex < thread_builtins.size())
      {
        auto it = thread_builtins[threadIndex].find(builtin);
        if(it != thread_builtins[threadIndex].end())
        {
          var.value = it->second.value;
          return;
        }
      }

      auto it = global_builtins.find(builtin);
      if(it != global_builtins.end())
      {
        var.value = it->second.value;
        return;
      }

      NBDERR("Couldn't get input for %s", ToStr(builtin).c_str());
      return;
    }

    if(threadIndex < location_inputs.size())
    {
      if(location < location_inputs[threadIndex].size())
      {
        if(var.rows == 1)
        {
          if(component + var.columns > 4)
            NBDERR("Unexpected component %u for column count %u", component, var.columns);

          for(uint8_t c = 0; c < var.columns; c++)
            copyComp(var, c, location_inputs[threadIndex][location], component + c);
        }
        else
        {
          NBDASSERTEQUAL(component, 0);
          for(uint8_t r = 0; r < var.rows; r++)
            for(uint8_t c = 0; c < var.columns; c++)
              copyComp(var, r * var.columns + c, location_inputs[threadIndex][location + c], r);
        }
        return;
      }
    }

    NBDERR("Couldn't get input for %s at thread=%u, location=%u, component=%u", var.name.c_str(),
           threadIndex, location, component);
  }

  uint32_t GetThreadProperty(uint32_t threadIndex, nbdspv::ThreadProperty prop) override
  {
    CHECK_DEVICE_THREAD();
    if(prop >= nbdspv::ThreadProperty::Count)
      return 0;
    if(threadIndex >= thread_props.size())
      return 0;

    return thread_props[threadIndex][(size_t)prop];
  }

  bool QueueSampleGather(nbdspv::ThreadState &lane, nbdspv::Op opcode,
                         DebugAPIWrapper::TextureType texType, const ShaderBindIndex &imageBind,
                         const ShaderBindIndex &samplerBind, const ShaderVariable &uv,
                         const ShaderVariable &ddxCalc, const ShaderVariable &ddyCalc,
                         const ShaderVariable &compare, nbdspv::GatherChannel gatherChannel,
                         const nbdspv::ImageOperandsAndParamDatas &operands, ShaderVariable &output,
                         bool &hasResult) override
  {
    CHECK_DEVICE_THREAD();
    ShaderConstParameters constParams = {};
    ShaderUniformParameters uniformParams = {};

    const bool buffer = (texType & DebugAPIWrapper::Buffer_Texture) != 0;
    const bool uintTex = (texType & DebugAPIWrapper::UInt_Texture) != 0;
    const bool sintTex = (texType & DebugAPIWrapper::SInt_Texture) != 0;

    // fetch the right type of descriptor depending on if we're buffer or not
    bool valid = true;
    nbdstr access = StringFormat::Fmt("performing %s operation", ToStr(opcode).c_str());
    const Descriptor &imageDescriptor = buffer ? GetDescriptor(access, ShaderBindIndex(), valid)
                                               : GetDescriptor(access, imageBind, valid);
    const Descriptor &bufferViewDescriptor = buffer
                                                 ? GetDescriptor(access, imageBind, valid)
                                                 : GetDescriptor(access, ShaderBindIndex(), valid);

    // fetch the sampler (if there's no sampler, this will silently return dummy data without
    // marking invalid
    const SamplerDescriptor &samplerDescriptor = GetSamplerDescriptor(access, samplerBind, valid);

    // if any descriptor lookup failed, return now
    if(!valid)
    {
      hasResult = false;
      return false;
    }

    VkMarkerRegion markerRegion("QueueSampleGather");

    VkBufferView bufferView =
        m_pDriver->GetResourceManager()->GetHandle<VkBufferView>(bufferViewDescriptor.view);

    VkSampler sampler =
        m_pDriver->GetResourceManager()->GetHandle<VkSampler>(samplerDescriptor.object);
    VkImageView view = m_pDriver->GetResourceManager()->GetHandle<VkImageView>(imageDescriptor.view);
    VkImageLayout layout = convert((DescriptorSlotImageLayout)imageDescriptor.byteOffset);

    // NULL view : return 0,0,0,0
    if(!buffer && (view == VK_NULL_HANDLE))
    {
      memset(&output.value, 0, sizeof(output.value));
      hasResult = true;
      return true;
    }

    // promote view to Array view
    VulkanCreationInfo::ImageView defaultViewProps = {};
    VulkanCreationInfo::Image defaultImageProps = {};
    VulkanCreationInfo::Buffer defaultBufferProps = {};
    VulkanCreationInfo::Sampler defaultSamplerProps = {};

    const VulkanCreationInfo::ImageView &viewProps =
        buffer ? defaultViewProps : m_Creation.GetImageViewInfo(GetResID(view));

    // NULL image : return 0,0,0,0
    if(!buffer && (viewProps.image == ResourceId()))
    {
      memset(&output.value, 0, sizeof(output.value));
      hasResult = true;
      return true;
    }

    const VulkanCreationInfo::Image &imageProps =
        buffer ? defaultImageProps : m_Creation.GetImageInfo(viewProps.image);

    const bool depthTex = IsDepthOrStencilFormat(viewProps.format);

    VkDevice dev = m_pDriver->GetDev();

    // how many co-ordinates should there be
    int coords = 0, gradCoords = 0;
    if(buffer)
    {
      constParams.dim = ShaderDebugBind::Buffer;
      coords = gradCoords = 1;
    }
    else
    {
      switch(viewProps.viewType)
      {
        case VK_IMAGE_VIEW_TYPE_1D:
          coords = 1;
          gradCoords = 1;
          constParams.dim = ShaderDebugBind::Tex1D;
          break;
        case VK_IMAGE_VIEW_TYPE_2D:
          coords = 2;
          gradCoords = 2;
          constParams.dim = ShaderDebugBind::Tex2D;
          break;
        case VK_IMAGE_VIEW_TYPE_3D:
          coords = 3;
          gradCoords = 3;
          constParams.dim = ShaderDebugBind::Tex3D;
          break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
          coords = 3;
          gradCoords = 3;
          constParams.dim = ShaderDebugBind::TexCube;
          break;
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
          coords = 2;
          gradCoords = 1;
          constParams.dim = ShaderDebugBind::Tex1D;
          break;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
          coords = 3;
          gradCoords = 2;
          constParams.dim = ShaderDebugBind::Tex2D;
          break;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
          coords = 4;
          gradCoords = 3;
          constParams.dim = ShaderDebugBind::TexCube;
          break;
        case VK_IMAGE_VIEW_TYPE_MAX_ENUM:
          NBDERR("Invalid image view type %s", ToStr(viewProps.viewType).c_str());
          return false;
      }

      if(imageProps.samples > 1)
        constParams.dim = ShaderDebugBind::Tex2DMS;
    }

    // handle query opcodes now
    switch(opcode)
    {
      case nbdspv::Op::ImageQueryLevels:
      {
        output.value.u32v[0] = viewProps.range.levelCount;
        if(viewProps.range.levelCount == VK_REMAINING_MIP_LEVELS)
          output.value.u32v[0] = imageProps.mipLevels - viewProps.range.baseMipLevel;
        hasResult = true;
        return true;
      }
      case nbdspv::Op::ImageQuerySamples:
      {
        output.value.u32v[0] = (uint32_t)imageProps.samples;
        hasResult = true;
        return true;
      }
      case nbdspv::Op::ImageQuerySize:
      case nbdspv::Op::ImageQuerySizeLod:
      {
        uint32_t mip = viewProps.range.baseMipLevel;

        if(opcode == nbdspv::Op::ImageQuerySizeLod)
          mip += uintComp(lane.GetSrc(operands.lod), 0);

        NBDEraseEl(output.value);

        int i = 0;
        setUintComp(output, i++, NBDMAX(1U, imageProps.extent.width >> mip));
        if(coords >= 2)
          setUintComp(output, i++, NBDMAX(1U, imageProps.extent.height >> mip));
        if(viewProps.viewType == VK_IMAGE_VIEW_TYPE_3D)
          setUintComp(output, i++, NBDMAX(1U, imageProps.extent.depth >> mip));

        if(viewProps.viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
           viewProps.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY)
          setUintComp(output, i++, imageProps.arrayLayers);
        else if(viewProps.viewType == VK_IMAGE_VIEW_TYPE_CUBE ||
                viewProps.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
          setUintComp(output, i++, imageProps.arrayLayers / 6);

        if(buffer)
        {
          VkDeviceSize size;
          VkFormat format;
          if(bufferView == VK_NULL_HANDLE)
          {
            // descriptor buffer case - there is no buffer view so read directly out of the determined descriptor
            format = MakeVkFormat(bufferViewDescriptor.format);
            // size is not allowed to be VK_WHOLE_SIZE
            size = bufferViewDescriptor.byteSize;
          }
          else
          {
            const VulkanCreationInfo::BufferView &bufViewProps =
                m_Creation.GetBufferViewInfo(GetResID(bufferView));

            size = bufViewProps.size;
            format = bufViewProps.format;

            if(size == VK_WHOLE_SIZE)
            {
              const VulkanCreationInfo::Buffer &bufProps =
                  bufViewProps.buffer == ResourceId()
                      ? defaultBufferProps
                      : m_Creation.GetBufferInfo(bufViewProps.buffer);
              size = bufProps.size - bufViewProps.offset;
            }
          }

          setUintComp(output, 0, uint32_t(size / GetByteSize(1, 1, 1, format, 0)));
        }

        hasResult = true;
        return true;
      }
      default: break;
    }

    // create our own view (if we haven't already for this view) so we can promote to array
    VkImageView sampleView = m_SampleViews[GetResID(view)];
    if(sampleView == VK_NULL_HANDLE && view != VK_NULL_HANDLE)
    {
      VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_pDriver->GetResourceManager()->GetHandle<VkImage>(viewProps.image);
      viewInfo.format = viewProps.format;
      viewInfo.viewType = viewProps.viewType;
      if(viewInfo.viewType == VK_IMAGE_VIEW_TYPE_1D)
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
      else if(viewInfo.viewType == VK_IMAGE_VIEW_TYPE_2D)
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      else if(viewInfo.viewType == VK_IMAGE_VIEW_TYPE_CUBE &&
              m_pDriver->GetDeviceEnabledFeatures().imageCubeArray)
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;

      viewInfo.components = viewProps.componentMapping;
      viewInfo.subresourceRange = viewProps.range;

      // if KHR_maintenance2 is available, ensure we have sampled usage available
      VkImageViewUsageCreateInfo usageCreateInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
      if(m_pDriver->GetExtensions(NULL).ext_KHR_maintenance2)
      {
        usageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        viewInfo.pNext = &usageCreateInfo;
      }

      VkResult vkr = m_pDriver->vkCreateImageView(dev, &viewInfo, NULL, &sampleView);
      CHECK_VKR(m_pDriver, vkr);

      m_SampleViews[GetResID(view)] = sampleView;
    }

    if(operands.flags & nbdspv::ImageOperands::Bias)
    {
      const ShaderVariable &biasVar = lane.GetSrc(operands.bias);

      // silently cast parameters to 32-bit floats
      float bias = floatComp(biasVar, 0);

      if(bias != 0.0f)
      {
        // bias can only be used with implicit lod operations, but we want to do everything with
        // explicit lod operations. So we instead push the bias into a new sampler, which is
        // entirely equivalent.

        // first check to see if we have one already, since the bias is probably going to be
        // coherent.
        SamplerBiasKey key = {GetResID(sampler), bias};

        auto insertIt = m_BiasSamplers.insert(std::make_pair(key, VkSampler()));
        if(insertIt.second)
        {
          const VulkanCreationInfo::Sampler &samplerProps =
              (sampler == VK_NULL_HANDLE) ? defaultSamplerProps
                                          : m_Creation.GetSamplerInfo(key.first);

          VkSamplerCreateInfo sampInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
          sampInfo.magFilter = samplerProps.magFilter;
          sampInfo.minFilter = samplerProps.minFilter;
          sampInfo.mipmapMode = samplerProps.mipmapMode;
          sampInfo.addressModeU = samplerProps.address[0];
          sampInfo.addressModeV = samplerProps.address[1];
          sampInfo.addressModeW = samplerProps.address[2];
          sampInfo.mipLodBias = samplerProps.mipLodBias;
          sampInfo.anisotropyEnable = samplerProps.maxAnisotropy >= 1.0f;
          sampInfo.maxAnisotropy = samplerProps.maxAnisotropy;
          sampInfo.compareEnable = samplerProps.compareEnable;
          sampInfo.compareOp = samplerProps.compareOp;
          sampInfo.minLod = samplerProps.minLod;
          sampInfo.maxLod = samplerProps.maxLod;
          sampInfo.borderColor = samplerProps.borderColor;
          sampInfo.unnormalizedCoordinates = samplerProps.unnormalizedCoordinates;

          VkSamplerReductionModeCreateInfo reductionInfo = {
              VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
          if(samplerProps.reductionMode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE)
          {
            reductionInfo.reductionMode = samplerProps.reductionMode;

            reductionInfo.pNext = sampInfo.pNext;
            sampInfo.pNext = &reductionInfo;
          }

          VkSamplerYcbcrConversionInfo ycbcrInfo = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
          if(samplerProps.ycbcr != ResourceId())
          {
            ycbcrInfo.conversion =
                m_pDriver->GetResourceManager()->GetHandle<VkSamplerYcbcrConversion>(viewProps.image);

            ycbcrInfo.pNext = sampInfo.pNext;
            sampInfo.pNext = &ycbcrInfo;
          }

          VkSamplerCustomBorderColorCreateInfoEXT borderInfo = {
              VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
          if(samplerProps.customBorder)
          {
            borderInfo.customBorderColor = samplerProps.customBorderColor;
            borderInfo.format = samplerProps.customBorderFormat;

            borderInfo.pNext = sampInfo.pNext;
            sampInfo.pNext = &borderInfo;
          }

          // now add the shader's bias on
          sampInfo.mipLodBias += bias;

          VkResult vkr = m_pDriver->vkCreateSampler(dev, &sampInfo, NULL, &sampler);
          CHECK_VKR(m_pDriver, vkr);

          insertIt.first->second = sampler;
        }
        else
        {
          sampler = insertIt.first->second;
        }
      }
    }

    constParams.operation = (uint32_t)opcode;

    // proj opcodes have an extra q parameter, but we do the divide ourselves and 'demote' these to
    // non-proj variants
    bool proj = false;
    switch(opcode)
    {
      case nbdspv::Op::ImageSampleProjExplicitLod:
      {
        constParams.operation = (uint32_t)nbdspv::Op::ImageSampleExplicitLod;
        proj = true;
        break;
      }
      case nbdspv::Op::ImageSampleProjImplicitLod:
      {
        constParams.operation = (uint32_t)nbdspv::Op::ImageSampleImplicitLod;
        proj = true;
        break;
      }
      case nbdspv::Op::ImageSampleProjDrefExplicitLod:
      {
        constParams.operation = (uint32_t)nbdspv::Op::ImageSampleDrefExplicitLod;
        proj = true;
        break;
      }
      case nbdspv::Op::ImageSampleProjDrefImplicitLod:
      {
        constParams.operation = (uint32_t)nbdspv::Op::ImageSampleDrefImplicitLod;
        proj = true;
        break;
      }
      default: break;
    }

    bool useCompare = false;
    switch(opcode)
    {
      case nbdspv::Op::ImageDrefGather:
      case nbdspv::Op::ImageSampleDrefExplicitLod:
      case nbdspv::Op::ImageSampleDrefImplicitLod:
      case nbdspv::Op::ImageSampleProjDrefExplicitLod:
      case nbdspv::Op::ImageSampleProjDrefImplicitLod:
      {
        useCompare = true;

        if(m_pDriver->GetDriverInfo().QualcommDrefNon2DCompileCrash() &&
           constParams.dim != ShaderDebugBind::Tex2D)
        {
          m_pDriver->AddDebugMessage(
              MessageCategory::Execution, MessageSeverity::High, MessageSource::RuntimeWarning,
              "Dref sample against non-2D texture, this cannot be debugged due to a driver bug");
        }

        break;
      }
      default: break;
    }

    bool gatherOp = false;

    switch(opcode)
    {
      case nbdspv::Op::ImageFetch:
      {
        // co-ordinates after the used ones are read as 0s. This allows us to then read an implicit
        // 0 for array layer when we promote accesses to arrays.
        uniformParams.texel_uvw.x = uintComp(uv, 0);
        if(coords >= 2)
          uniformParams.texel_uvw.y = uintComp(uv, 1);
        if(coords >= 3)
          uniformParams.texel_uvw.z = uintComp(uv, 2);

        if(!buffer && operands.flags & nbdspv::ImageOperands::Lod)
          uniformParams.texel_lod = uintComp(lane.GetSrc(operands.lod), 0);
        else
          uniformParams.texel_lod = 0;

        if(operands.flags & nbdspv::ImageOperands::Sample)
          uniformParams.sampleIdx = uintComp(lane.GetSrc(operands.sample), 0);

        break;
      }
      case nbdspv::Op::ImageGather:
      case nbdspv::Op::ImageDrefGather:
      {
        gatherOp = true;

        // silently cast parameters to 32-bit floats
        for(int i = 0; i < coords; i++)
          uniformParams.uvwa[i] = floatComp(uv, i);

        if(useCompare)
          uniformParams.compare = floatComp(compare, 0);

        constParams.gatherChannel = gatherChannel;

        if(operands.flags & nbdspv::ImageOperands::ConstOffsets)
        {
          ShaderVariable constOffsets = lane.GetSrc(operands.constOffsets);

          constParams.useGradOrGatherOffsets = VK_TRUE;

          // should be an array of ivec2
          NBDASSERT(constOffsets.members.size() == 4);

          // sign extend variables lower than 32-bits
          for(int i = 0; i < 4; i++)
          {
            if(constOffsets.members[i].type == VarType::SByte)
            {
              constOffsets.members[i].value.s32v[0] = constOffsets.members[i].value.s8v[0];
              constOffsets.members[i].value.s32v[1] = constOffsets.members[i].value.s8v[1];
            }
            else if(constOffsets.members[i].type == VarType::SShort)
            {
              constOffsets.members[i].value.s32v[0] = constOffsets.members[i].value.s16v[0];
              constOffsets.members[i].value.s32v[1] = constOffsets.members[i].value.s16v[1];
            }
          }

          constParams.gatherOffsets.u0 = constOffsets.members[0].value.s32v[0];
          constParams.gatherOffsets.v0 = constOffsets.members[0].value.s32v[1];
          constParams.gatherOffsets.u1 = constOffsets.members[1].value.s32v[0];
          constParams.gatherOffsets.v1 = constOffsets.members[1].value.s32v[1];
          constParams.gatherOffsets.u2 = constOffsets.members[2].value.s32v[0];
          constParams.gatherOffsets.v2 = constOffsets.members[2].value.s32v[1];
          constParams.gatherOffsets.u3 = constOffsets.members[3].value.s32v[0];
          constParams.gatherOffsets.v3 = constOffsets.members[3].value.s32v[1];
        }

        break;
      }
      case nbdspv::Op::ImageQueryLod:
      case nbdspv::Op::ImageSampleExplicitLod:
      case nbdspv::Op::ImageSampleImplicitLod:
      case nbdspv::Op::ImageSampleProjExplicitLod:
      case nbdspv::Op::ImageSampleProjImplicitLod:
      case nbdspv::Op::ImageSampleDrefExplicitLod:
      case nbdspv::Op::ImageSampleDrefImplicitLod:
      case nbdspv::Op::ImageSampleProjDrefExplicitLod:
      case nbdspv::Op::ImageSampleProjDrefImplicitLod:
      {
        // silently cast parameters to 32-bit floats
        for(int i = 0; i < coords; i++)
          uniformParams.uvwa[i] = floatComp(uv, i);

        if(proj)
        {
          // coords shouldn't be 4 because that's only valid for cube arrays which can't be
          // projected
          NBDASSERT(coords < 4);

          // do the divide ourselves rather than severely complicating the sample shader (as proj
          // variants need non-arrayed textures)
          float q = floatComp(uv, coords);

          uniformParams.uvwa[0] /= q;
          uniformParams.uvwa[1] /= q;
          uniformParams.uvwa[2] /= q;
        }

        if(operands.flags & nbdspv::ImageOperands::MinLod)
        {
          const ShaderVariable &minLodVar = lane.GetSrc(operands.minLod);

          // silently cast parameters to 32-bit floats
          uniformParams.minlod = floatComp(minLodVar, 0);
        }

        if(useCompare)
        {
          // silently cast parameters to 32-bit floats
          uniformParams.compare = floatComp(compare, 0);
        }

        if(operands.flags & nbdspv::ImageOperands::Lod)
        {
          const ShaderVariable &lodVar = lane.GetSrc(operands.lod);

          // silently cast parameters to 32-bit floats
          uniformParams.lod = floatComp(lodVar, 0);
          constParams.useGradOrGatherOffsets = VK_FALSE;
        }
        else if(operands.flags & nbdspv::ImageOperands::Grad)
        {
          ShaderVariable ddx = lane.GetSrc(operands.grad.first);
          ShaderVariable ddy = lane.GetSrc(operands.grad.second);

          constParams.useGradOrGatherOffsets = VK_TRUE;

          // silently cast parameters to 32-bit floats
          NBDASSERTEQUAL(ddx.type, ddy.type);
          for(int i = 0; i < gradCoords; i++)
          {
            uniformParams.ddx[i] = floatComp(ddx, i);
            uniformParams.ddy[i] = floatComp(ddy, i);
          }
        }

        if(opcode == nbdspv::Op::ImageSampleImplicitLod ||
           opcode == nbdspv::Op::ImageSampleProjImplicitLod || opcode == nbdspv::Op::ImageQueryLod)
        {
          // use grad to sub in for the implicit lod
          constParams.useGradOrGatherOffsets = VK_TRUE;

          // silently cast parameters to 32-bit floats
          NBDASSERTEQUAL(ddxCalc.type, ddyCalc.type);
          for(int i = 0; i < gradCoords; i++)
          {
            uniformParams.ddx[i] = floatComp(ddxCalc, i);
            uniformParams.ddy[i] = floatComp(ddyCalc, i);
          }
        }
        if((opcode == nbdspv::Op::ImageSampleProjDrefExplicitLod) ||
           (opcode == nbdspv::Op::ImageSampleProjDrefImplicitLod))
        {
          NBDASSERT(useCompare);
          float q = floatComp(uv, coords);
          uniformParams.compare /= q;
        }

        break;
      }
      default:
      {
        NBDERR("Unsupported opcode %s", ToStr(opcode).c_str());
        return false;
      }
    }

    if(operands.flags & nbdspv::ImageOperands::ConstOffset)
    {
      ShaderVariable constOffset = lane.GetSrc(operands.constOffset);

      // sign extend variables lower than 32-bits
      for(uint8_t c = 0; c < constOffset.columns; c++)
      {
        if(constOffset.type == VarType::SByte)
          constOffset.value.s32v[c] = constOffset.value.s8v[c];
        else if(constOffset.type == VarType::SShort)
          constOffset.value.s32v[c] = constOffset.value.s16v[c];
      }

      // pass offsets as uniform where possible - when the feature (widely available) on gather
      // operations. On non-gather operations we are forced to use const offsets and must specialise
      // the pipeline.
      if(m_pDriver->GetDeviceEnabledFeatures().shaderImageGatherExtended && gatherOp)
      {
        uniformParams.offset.x = constOffset.value.s32v[0];
        if(gradCoords >= 2)
          uniformParams.offset.y = constOffset.value.s32v[1];
        if(gradCoords >= 3)
          uniformParams.offset.z = constOffset.value.s32v[2];
      }
      else
      {
        constParams.constOffsets.x = constOffset.value.s32v[0];
        if(gradCoords >= 2)
          constParams.constOffsets.y = constOffset.value.s32v[1];
        if(gradCoords >= 3)
          constParams.constOffsets.z = constOffset.value.s32v[2];
      }
    }
    else if(operands.flags & nbdspv::ImageOperands::Offset)
    {
      ShaderVariable offset = lane.GetSrc(operands.offset);

      // sign extend variables lower than 32-bits
      for(uint8_t c = 0; c < offset.columns; c++)
      {
        if(offset.type == VarType::SByte)
          offset.value.s32v[c] = offset.value.s8v[c];
        else if(offset.type == VarType::SShort)
          offset.value.s32v[c] = offset.value.s16v[c];
      }

      // if the app's shader used a dynamic offset, we can too!
      uniformParams.offset.x = offset.value.s32v[0];
      if(gradCoords >= 2)
        uniformParams.offset.y = offset.value.s32v[1];
      if(gradCoords >= 3)
        uniformParams.offset.z = offset.value.s32v[2];
    }

    if(!m_pDriver->GetDeviceEnabledFeatures().shaderImageGatherExtended &&
       (uniformParams.offset.x != 0 || uniformParams.offset.y != 0 || uniformParams.offset.z != 0))
    {
      m_pDriver->AddDebugMessage(
          MessageCategory::Execution, MessageSeverity::High, MessageSource::RuntimeWarning,
          StringFormat::Fmt("Use of constant offsets %d/%d/%d is not supported without "
                            "shaderImageGatherExtended device feature",
                            uniformParams.offset.x, uniformParams.offset.y, uniformParams.offset.z));
    }

    VkPipeline pipe = MakePipe(constParams, 32, depthTex, uintTex, sintTex);

    if(pipe == VK_NULL_HANDLE)
    {
      m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                 MessageSource::RuntimeWarning,
                                 "Failed to compile graphics pipeline for sampling operation");
      return false;
    }

    VkCommandBuffer cmd = queuedOpCmdBuffer;
    if(cmd == VK_NULL_HANDLE)
    {
      if(StartQueuedOps())
        cmd = queuedOpCmdBuffer;
    }
    if(cmd == VK_NULL_HANDLE)
      return false;

    if(queueIndex >= ShaderDebugData::MAX_QUEUED_OPS)
    {
      m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                 MessageSource::RuntimeWarning, "Too many GPU queued operations");
      return false;
    }

    VkDescriptorImageInfo samplerWriteInfo = {Unwrap(sampler), VK_NULL_HANDLE,
                                              VK_IMAGE_LAYOUT_UNDEFINED};
    VkDescriptorImageInfo imageWriteInfo = {VK_NULL_HANDLE, Unwrap(sampleView), layout};

    VkDescriptorBufferInfo uniformWriteInfo = {};
    m_DebugData.ConstantsBuffer.FillDescriptor(uniformWriteInfo);
    const VkDescriptorSet realDescSet = Unwrap(m_DebugData.DescSets[queueIndex++]);

    VkWriteDescriptorSet writeSets[] = {
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            NULL,
            realDescSet,
            (uint32_t)ShaderDebugBind::Constants,
            0,
            1,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            NULL,
            &uniformWriteInfo,
            NULL,
        },
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            NULL,
            realDescSet,
            (uint32_t)constParams.dim,
            0,
            1,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            &imageWriteInfo,
            NULL,
            NULL,
        },
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            NULL,
            realDescSet,
            (uint32_t)ShaderDebugBind::Sampler,
            0,
            1,
            VK_DESCRIPTOR_TYPE_SAMPLER,
            &samplerWriteInfo,
            NULL,
            NULL,
        },
    };

    if(buffer)
    {
      if(bufferView == VK_NULL_HANDLE)
      {
        // descriptor buffer, must create our own

        BufViewKey key = {bufferViewDescriptor.resource, bufferViewDescriptor.byteOffset,
                          bufferViewDescriptor.byteSize, MakeVkFormat(bufferViewDescriptor.format)};

        bufferView = m_SampleBufViews[key];
        if(bufferView == VK_NULL_HANDLE)
        {
          VkBufferViewCreateInfo viewInfo = {
              VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
              NULL,
              0,
              m_pDriver->GetResourceManager()->GetHandle<VkBuffer>(bufferViewDescriptor.resource),
              key.format,
              bufferViewDescriptor.byteOffset,
              bufferViewDescriptor.byteSize,
          };

          VkResult vkr = m_pDriver->vkCreateBufferView(dev, &viewInfo, NULL, &bufferView);
          CHECK_VKR(m_pDriver, vkr);

          m_SampleBufViews[key] = bufferView;
        }
      }

      writeSets[1].pTexelBufferView = UnwrapPtr(bufferView);
      writeSets[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    }

    // reset descriptor sets to dummy state
    if(depthTex)
    {
      uint32_t resetIndex = 3;

      nbdarray<VkWriteDescriptorSet> writes;

      for(size_t i = 0; i < ARRAY_COUNT(m_DebugData.DummyWrites[resetIndex]); i++)
      {
        // not all textures may be supported for depth, so only update those that are valid
        if(m_DebugData.DummyWrites[resetIndex][i].descriptorCount != 0)
        {
          m_DebugData.DummyWrites[resetIndex][i].dstSet = realDescSet;
          writes.push_back(m_DebugData.DummyWrites[resetIndex][i]);
        }
      }

      ObjDisp(dev)->UpdateDescriptorSets(Unwrap(dev), (uint32_t)writes.count(), writes.data(), 0,
                                         NULL);
    }
    else
    {
      uint32_t resetIndex = 0;
      if(uintTex)
        resetIndex = 1;
      else if(sintTex)
        resetIndex = 2;

      for(size_t i = 0; i < ARRAY_COUNT(m_DebugData.DummyWrites[resetIndex]); i++)
      {
        if(m_DebugData.DummyWrites[resetIndex][i].descriptorCount != 0)
          m_DebugData.DummyWrites[resetIndex][i].dstSet = realDescSet;
      }
      ObjDisp(dev)->UpdateDescriptorSets(Unwrap(dev),
                                         ARRAY_COUNT(m_DebugData.DummyWrites[resetIndex]),
                                         m_DebugData.DummyWrites[resetIndex], 0, NULL);
    }

    // overwrite with our data
    ObjDisp(dev)->UpdateDescriptorSets(Unwrap(dev), sampler != VK_NULL_HANDLE ? 3 : 2, writeSets, 0,
                                       NULL);

    uint32_t constsOffset = 0;
    void *constants = m_DebugData.ConstantsBuffer.Map(&constsOffset, 0);
    if(!constants)
      return false;

    memcpy(constants, &uniformParams, sizeof(uniformParams));

    m_DebugData.ConstantsBuffer.Unmap();

    VkClearValue clear = {};

    VkRenderPassBeginInfo rpbegin = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        NULL,
        Unwrap(m_DebugData.RenderPass),
        Unwrap(m_DebugData.Framebuffer),
        {{0, 0}, {1, 1}},
        1,
        &clear,
    };
    ObjDisp(cmd)->CmdBeginRenderPass(Unwrap(cmd), &rpbegin, VK_SUBPASS_CONTENTS_INLINE);

    ObjDisp(cmd)->CmdBindPipeline(Unwrap(cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, Unwrap(pipe));
    ObjDisp(cmd)->CmdBindDescriptorSets(Unwrap(cmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        Unwrap(m_DebugData.PipeLayout), 0, 1, &realDescSet, 1,
                                        &constsOffset);

    // push uvw/ddx/ddy for the vertex shader
    ObjDisp(cmd)->CmdPushConstants(Unwrap(cmd), Unwrap(m_DebugData.PipeLayout), VK_SHADER_STAGE_ALL,
                                   sizeof(Vec4f) * 0, sizeof(Vec4f), &uniformParams.uvwa);
    ObjDisp(cmd)->CmdPushConstants(Unwrap(cmd), Unwrap(m_DebugData.PipeLayout), VK_SHADER_STAGE_ALL,
                                   sizeof(Vec4f) * 1, sizeof(Vec3f), &uniformParams.ddx);
    ObjDisp(cmd)->CmdPushConstants(Unwrap(cmd), Unwrap(m_DebugData.PipeLayout), VK_SHADER_STAGE_ALL,
                                   sizeof(Vec4f) * 2, sizeof(Vec3f), &uniformParams.ddy);

    ObjDisp(cmd)->CmdDraw(Unwrap(cmd), 3, 1, 0, 0);

    ObjDisp(cmd)->CmdEndRenderPass(Unwrap(cmd));

    NBDASSERT(sampleGatherOpResultOffset + sampleGatherOpResultByteSize <=
                  m_DebugData.ReadbackBuffer.TotalSize(),
              sampleGatherOpResultOffset, sampleGatherOpResultByteSize,
              m_DebugData.ReadbackBuffer.TotalSize());
    VkBufferImageCopy region = {
        sampleGatherOpResultOffset,           sizeof(Vec4f), 1,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},     {1, 1, 1},
    };
    sampleGatherOpResultOffset += sampleGatherOpResultByteSize;
    ObjDisp(cmd)->CmdCopyImageToBuffer(Unwrap(cmd), Unwrap(m_DebugData.Image),
                                       VK_IMAGE_LAYOUT_GENERAL,
                                       m_DebugData.ReadbackBuffer.UnwrappedBuffer(), 1, &region);

    hasResult = false;
    return true;
  }

  virtual bool QueueCalculateMathOp(nbdspv::Op opcode, nbdspv::GLSLstd450 glslop,
                                    const nbdarray<ShaderVariable> &params) override
  {
    CHECK_DEVICE_THREAD();
    NBDASSERT(params.size() <= 3, params.size());

    int floatSizeIdx = 0;
    if(params[0].type == VarType::Half)
      floatSizeIdx = 1;
    else if(params[0].type == VarType::Double)
      floatSizeIdx = 2;

    if(m_DebugData.MathPipe[floatSizeIdx] == VK_NULL_HANDLE)
    {
      ShaderConstParameters pipeParams = {};
      pipeParams.operation = (uint32_t)nbdspv::Op::ExtInst;
      m_DebugData.MathPipe[floatSizeIdx] =
          MakePipe(pipeParams, VarTypeByteSize(params[0].type) * 8, false, false, false);

      if(m_DebugData.MathPipe[floatSizeIdx] == VK_NULL_HANDLE)
      {
        m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                   MessageSource::RuntimeWarning,
                                   "Failed to compile graphics pipeline for math operation");
        return false;
      }
    }

    VkCommandBuffer cmd = queuedOpCmdBuffer;
    if(cmd == VK_NULL_HANDLE)
    {
      if(StartQueuedOps())
        cmd = queuedOpCmdBuffer;
    }
    if(cmd == VK_NULL_HANDLE)
      return false;

    if(queueIndex >= ShaderDebugData::MAX_QUEUED_OPS)
    {
      m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                 MessageSource::RuntimeWarning, "Too many GPU queued operations");
      return false;
    }
    VkDescriptorBufferInfo storageWriteInfo = {};
    m_DebugData.MathResult.FillDescriptor(storageWriteInfo);

    const VkDescriptorSet realDescSet = Unwrap(m_DebugData.DescSets[queueIndex++]);

    VkWriteDescriptorSet writeSets[] = {
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            NULL,
            realDescSet,
            (uint32_t)ShaderDebugBind::MathResult,
            0,
            1,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            NULL,
            &storageWriteInfo,
            NULL,
        },
    };

    VkDevice dev = m_pDriver->GetDev();

    ObjDisp(dev)->UpdateDescriptorSets(Unwrap(dev), 1, writeSets, 0, NULL);

    VkMarkerRegion markerRegion("QueueCalculateMathOp");

    ObjDisp(cmd)->CmdBindPipeline(Unwrap(cmd), VK_PIPELINE_BIND_POINT_COMPUTE,
                                  Unwrap(m_DebugData.MathPipe[floatSizeIdx]));

    uint32_t constsOffset = 0;
    ObjDisp(cmd)->CmdBindDescriptorSets(Unwrap(cmd), VK_PIPELINE_BIND_POINT_COMPUTE,
                                        Unwrap(m_DebugData.PipeLayout), 0, 1, &realDescSet, 1,
                                        &constsOffset);

    // push the parameters
    for(size_t i = 0; i < params.size(); i++)
    {
      NBDASSERTEQUAL(params[i].type, params[0].type);
      double p[4] = {};
      memcpy(p, params[i].value.f32v.data(), VarTypeByteSize(params[i].type) * params[i].columns);
      ObjDisp(cmd)->CmdPushConstants(Unwrap(cmd), Unwrap(m_DebugData.PipeLayout),
                                     VK_SHADER_STAGE_ALL, uint32_t(sizeof(p) * i), sizeof(p), p);
    }

    // push the operation afterwards

    if(glslop == nbdspv::GLSLstd450::Invalid)
    {
      NBDCOMPILE_ASSERT(nbdspv::GLSLstd450::Max < (nbdspv::GLSLstd450)1000,
                        "GLSL std450 ops max is higher than expected");
      glslop = (nbdspv::GLSLstd450)(1000 + (uint32_t)opcode);
    }

    ObjDisp(cmd)->CmdPushConstants(Unwrap(cmd), Unwrap(m_DebugData.PipeLayout), VK_SHADER_STAGE_ALL,
                                   sizeof(Vec4f) * 6, sizeof(uint32_t), &glslop);

    ObjDisp(cmd)->CmdDispatch(Unwrap(cmd), 1, 1, 1);

    VkBufferMemoryBarrier bufBarrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        NULL,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_DebugData.MathResult.UnwrappedBuffer(),
        0,
        VK_WHOLE_SIZE,
    };

    DoPipelineBarrier(cmd, 1, &bufBarrier);

    NBDASSERT(mathOpResultOffset + mathOpResultByteSize <= m_DebugData.ReadbackBuffer.TotalSize(),
              mathOpResultOffset, mathOpResultByteSize, m_DebugData.ReadbackBuffer.TotalSize());
    VkBufferCopy bufCopy = {0, mathOpResultOffset, mathOpResultByteSize};
    mathOpResultOffset += mathOpResultByteSize;
    ObjDisp(cmd)->CmdCopyBuffer(Unwrap(cmd), m_DebugData.MathResult.UnwrappedBuffer(),
                                m_DebugData.ReadbackBuffer.UnwrappedBuffer(), 1, &bufCopy);
    return true;
  }

  bool StartQueuedOps()
  {
    CHECK_DEVICE_THREAD();

    NBDASSERTEQUAL(queueIndex, 0);
    NBDASSERTEQUAL(queuedOpCmdBuffer, VK_NULL_HANDLE);
    NBDASSERTEQUAL(mathOpResultOffset, 0);
    NBDASSERTEQUAL(sampleGatherOpResultOffset, 0);

    if(queuedOpCmdBuffer != VK_NULL_HANDLE)
      return false;

    queuedOpCmdBuffer = m_pDriver->GetNextCmd();
    if(queuedOpCmdBuffer == VK_NULL_HANDLE)
      return false;

    VkCommandBuffer cmd = queuedOpCmdBuffer;
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                          VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

    VkResult vkr = ObjDisp(cmd)->BeginCommandBuffer(Unwrap(cmd), &beginInfo);
    CHECK_VKR(m_pDriver, vkr);

    sampleGatherOpResultOffset = sampleGatherOpResultsStart;
    return true;
  }

  virtual bool GetQueuedResults(nbdarray<ShaderVariable *> &mathOpResults,
                                nbdarray<ShaderVariable *> &sampleGatherResults) override
  {
    CHECK_DEVICE_THREAD();
    if(queuedOpCmdBuffer == VK_NULL_HANDLE)
      return false;

    VkBufferMemoryBarrier bufBarrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        NULL,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_HOST_READ_BIT,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_DebugData.ReadbackBuffer.UnwrappedBuffer(),
        0,
        VK_WHOLE_SIZE,
    };

    VkCommandBuffer cmd = queuedOpCmdBuffer;
    if(cmd == VK_NULL_HANDLE)
      return false;

    // wait for copy to finish before reading back to host
    DoPipelineBarrier(cmd, 1, &bufBarrier);

    VkResult vkr = ObjDisp(cmd)->EndCommandBuffer(Unwrap(cmd));
    CHECK_VKR(m_pDriver, vkr);

    m_pDriver->SubmitCmds();
    m_pDriver->FlushQ();

    queueIndex = 0;
    queuedOpCmdBuffer = VK_NULL_HANDLE;
    mathOpResultOffset = 0;
    sampleGatherOpResultOffset = 0;

    byte *gpuResults = (byte *)m_DebugData.ReadbackBuffer.Map(NULL, 0);
    if(!gpuResults)
      return false;

    uintptr_t bufferEnd = (uintptr_t)(gpuResults + m_DebugData.ReadbackBuffer.TotalSize());

    byte *gpuMathOpResults = gpuResults;
    for(ShaderVariable *result : mathOpResults)
    {
      size_t countBytes = VarTypeByteSize(result->type) * result->columns;
      NBDASSERT((uintptr_t)gpuMathOpResults + countBytes <= bufferEnd, (uintptr_t)gpuMathOpResults,
                countBytes, bufferEnd);
      NBDASSERT(countBytes <= mathOpResultByteSize, countBytes, mathOpResultByteSize);
      memcpy(result->value.u32v.data(), gpuMathOpResults, countBytes);
      gpuMathOpResults += mathOpResultByteSize;
    }

    byte *gpuSampleGatherOpResults = gpuResults + sampleGatherOpResultsStart;
    for(ShaderVariable *result : sampleGatherResults)
    {
      float *retf = (float *)gpuSampleGatherOpResults;
      uint32_t *retu = (uint32_t *)gpuSampleGatherOpResults;
      int32_t *reti = (int32_t *)gpuSampleGatherOpResults;

      size_t countBytes = 16;
      NBDASSERT((uintptr_t)gpuSampleGatherOpResults + countBytes <= bufferEnd,
                (uintptr_t)gpuSampleGatherOpResults, countBytes, bufferEnd);
      NBDASSERT(countBytes <= sampleGatherOpResultByteSize, countBytes, sampleGatherOpResultByteSize);
      // convert full precision results, we did all sampling at 32-bit precision
      ShaderVariable &output = *result;
      for(uint8_t c = 0; c < 4; c++)
      {
        if(VarTypeCompType(output.type) == CompType::Float)
          setFloatComp(output, c, retf[c]);
        else if(VarTypeCompType(output.type) == CompType::SInt)
          setIntComp(output, c, reti[c]);
        else
          setUintComp(output, c, retu[c]);
      }
      gpuSampleGatherOpResults += sampleGatherOpResultByteSize;
    }

    m_DebugData.ReadbackBuffer.Unmap();
    return true;
  }

  virtual bool QueuedOpsHasSpace() override { return queueIndex < ShaderDebugData::MAX_QUEUED_OPS; }

  // Device thread only for mutable state
  std::unordered_map<ShaderBuiltin, ShaderVariable> &GetGlobalBuiltins()
  {
    CHECK_DEVICE_THREAD();
    if(inputVarsReadOnly)
      NBDERR("Input variables can't be modified");
    return global_builtins;
  }

  // Device thread only for mutable state
  nbdarray<std::unordered_map<ShaderBuiltin, ShaderVariable>> &GetThreadBuiltins()
  {
    CHECK_DEVICE_THREAD();
    if(inputVarsReadOnly)
      NBDERR("Input variables can't be modified");
    return thread_builtins;
  }

  // Device thread only for mutable state
  nbdarray<nbdarray<ShaderVariable>> &GetLocationInputs()
  {
    CHECK_DEVICE_THREAD();
    if(inputVarsReadOnly)
      NBDERR("Input variables can't be modified");
    return location_inputs;
  }

  // Device thread only for mutable state
  void SetInputVarsToReadOnly()
  {
    CHECK_DEVICE_THREAD();
    inputVarsReadOnly = true;
  }

  nbdarray<nbdfixedarray<uint32_t, arraydim<nbdspv::ThreadProperty>()>> thread_props;

  uint64_t GetDeviceThreadID() const { return deviceThreadID; }
  bool IsDeviceThread() const { return Threading::GetCurrentID() == GetDeviceThreadID(); }

private:
  WrappedVulkan *m_pDriver = NULL;
  ShaderDebugData &m_DebugData;
  const VulkanCreationInfo &m_Creation;

  bool m_ResourcesDirty = false;
  uint32_t m_EventID;
  ResourceId m_ShaderID;

  bool inputVarsReadOnly = false;
  // global over all threads
  std::unordered_map<ShaderBuiltin, ShaderVariable> global_builtins;
  // per-thread builtins
  nbdarray<std::unordered_map<ShaderBuiltin, ShaderVariable>> thread_builtins;
  // per-thread custom inputs by location [thread][location]
  nbdarray<nbdarray<ShaderVariable>> location_inputs;

  nbdarray<DescriptorAccess> m_Access;
  nbdarray<Descriptor> m_Descriptors;
  nbdarray<SamplerDescriptor> m_SamplerDescriptors;

  std::map<ResourceId, VkImageView> m_SampleViews;
  struct BufViewKey
  {
    ResourceId buf;
    VkDeviceSize offs, size;
    VkFormat format;
    bool operator==(const BufViewKey &o) const
    {
      return buf == o.buf && offs == o.offs && size == o.size && format == o.format;
    }
    bool operator<(const BufViewKey &o) const
    {
      if(buf != o.buf)
        return buf < o.buf;
      if(offs != o.offs)
        return offs < o.offs;
      if(size != o.size)
        return size < o.size;
      if(format != o.format)
        return format < o.format;
      return false;
    }
  };
  nbdflatmap<BufViewKey, VkBufferView> m_SampleBufViews;

  typedef nbdpair<ResourceId, float> SamplerBiasKey;
  std::map<SamplerBiasKey, VkSampler> m_BiasSamplers;

  Threading::RWLock bufferCacheLock;
  std::map<ShaderBindIndex, bytebuf> bufferCache;

  VkCommandBuffer queuedOpCmdBuffer = VK_NULL_HANDLE;
  VkDeviceSize mathOpResultOffset = 0;
  VkDeviceSize sampleGatherOpResultOffset = 0;
  uint32_t queueIndex = 0;
  const VkDeviceSize mathOpResultByteSize = sizeof(Vec4f) * 2;
  const VkDeviceSize sampleGatherOpResultByteSize = sizeof(Vec4f);
  const VkDeviceSize sampleGatherOpResultsStart =
      ShaderDebugData::MAX_QUEUED_OPS * mathOpResultByteSize;

  struct ImageData
  {
    uint32_t width = 0, height = 0, depth = 0;
    uint32_t texelSize = 0;
    uint64_t rowPitch = 0, slicePitch = 0, samplePitch = 0;
    ResourceFormat fmt;
    bytebuf bytes;

    byte *texel(const uint32_t *coord, uint32_t sample)
    {
      byte *ret = bytes.data();

      ret += samplePitch * sample;
      ret += slicePitch * coord[2];
      ret += rowPitch * coord[1];
      ret += texelSize * coord[0];

      return ret;
    }
  };

  Threading::RWLock imageCacheLock;
  std::map<ShaderBindIndex, ImageData> imageCache;

  const Descriptor &GetDescriptor(const nbdstr &access, const ShaderBindIndex &index, bool &valid)
  {
    CHECK_DEVICE_THREAD();
    static Descriptor dummy;

    if(index.category == DescriptorCategory::Unknown)
    {
      // invalid index, return a dummy data but don't mark as invalid
      return dummy;
    }

    int32_t a = m_Access.indexOf(index);

    // this should not happen unless the debugging references an array element that we didn't
    // detect dynamically. We could improve this by retrieving a more conservative access set
    // internally so that all descriptors are 'accessed'
    if(a < 0)
    {
      m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                 MessageSource::RuntimeWarning,
                                 StringFormat::Fmt("Internal error: Binding %s %u[%u] did not "
                                                   "exist in calculated descriptor access when %s.",
                                                   ToStr(index.category).c_str(), index.index,
                                                   index.arrayElement, access.c_str()));
      valid = false;
      return dummy;
    }

    return m_Descriptors[a];
  }

  const SamplerDescriptor &GetSamplerDescriptor(const nbdstr &access, const ShaderBindIndex &index,
                                                bool &valid)
  {
    CHECK_DEVICE_THREAD();
    static SamplerDescriptor dummy;

    if(index.category == DescriptorCategory::Unknown)
    {
      // invalid index, return a dummy data but don't mark as invalid
      return dummy;
    }

    int32_t a = m_Access.indexOf(index);

    // this should not happen unless the debugging references an array element that we didn't
    // detect dynamically. We could improve this by retrieving a more conservative access set
    // internally so that all descriptors are 'accessed'
    if(a < 0)
    {
      m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                 MessageSource::RuntimeWarning,
                                 StringFormat::Fmt("Internal error: Binding %s %u[%u] did not "
                                                   "exist in calculated descriptor access when %s.",
                                                   ToStr(index.category).c_str(), index.index,
                                                   index.arrayElement, access.c_str()));
      valid = false;
      return dummy;
    }

    return m_SamplerDescriptors[a];
  }

  // Called from any thread
  ShaderBindIndex GenerateBufferBind(const uint64_t address, size_t &offs)
  {
    ResourceId id;
    uint64_t ptrOffs;
    m_pDriver->GetResIDFromAddr(address, id, ptrOffs);
    offs = size_t(ptrOffs);

    ShaderBindIndex bind;
    bind.arrayElement = (address - offs) & 0xFFFFFFFFU;

    return bind;
  }

  // Called from any thread
  bool IsBufferCached(const ShaderBindIndex &bind) override
  {
    SCOPED_READLOCK(bufferCacheLock);
    return bufferCache.find(bind) != bufferCache.end();
  }

  // Called from any thread
  bool IsBufferCached(uint64_t address) override
  {
    size_t offs = 0;
    ShaderBindIndex bind = GenerateBufferBind(address, offs);
    return IsBufferCached(bind);
  }

  // Called from any thread
  bytebuf *GetBufferDataFromCache(const ShaderBindIndex &bind, nbdspv::DeviceOpResult &opResult)
  {
    // Calling function responsible for acquiring bufferCache Read lock
    auto findIt = bufferCache.find(bind);
    if(findIt != bufferCache.end())
    {
      opResult = nbdspv::DeviceOpResult::Succeeded;
      return &findIt->second;
    }

    opResult = nbdspv::DeviceOpResult::Failed;

    // Not in the cache : populate must happen on the device thread
    if(!IsDeviceThread())
      opResult = nbdspv::DeviceOpResult::NeedsDevice;

    return NULL;
  }

  // Called from any thread
  bool BufferFunction(const ShaderBindIndex &bind, const std::function<void(bytebuf *data)> &func,
                      nbdspv::DeviceOpResult &opResult)
  {
    bool isCached = false;
    {
      SCOPED_READLOCK(bufferCacheLock);
      isCached = GetBufferDataFromCache(bind, opResult) != NULL;
      if(opResult == nbdspv::DeviceOpResult::NeedsDevice)
        return false;
    }

    if(!isCached)
    {
      // Add buffer data to the cache : cache should not be locked by this thread
      PopulateBuffer(bind);
    }

    {
      SCOPED_READLOCK(bufferCacheLock);
      bytebuf *result = GetBufferDataFromCache(bind, opResult);
      if(result)
      {
        // Guarantee the buffer cache readlock whilst the function is called
        func(result);
        return true;
      }

      NBDASSERTEQUAL(opResult, nbdspv::DeviceOpResult::Failed);
      opResult = nbdspv::DeviceOpResult::Failed;
      return false;
    }
  }

  // Called from any thread
  bool BufferFunction(uint64_t address, const std::function<void(bytebuf *data, size_t offset)> &func,
                      nbdspv::DeviceOpResult &opResult)
  {
    size_t offs = 0;
    ShaderBindIndex bind = GenerateBufferBind(address, offs);
    bool isCached = false;
    {
      SCOPED_READLOCK(bufferCacheLock);
      isCached = GetBufferDataFromCache(bind, opResult) != NULL;
      if(opResult == nbdspv::DeviceOpResult::NeedsDevice)
        return false;
    }

    if(!isCached)
    {
      // Add buffer data to the cache : cache should not be locked by this thread
      PopulateBuffer(address, bind);
    }

    {
      SCOPED_READLOCK(bufferCacheLock);
      bytebuf *result = GetBufferDataFromCache(bind, opResult);
      if(result)
      {
        // Guarantee the buffer cache readlock whilst the function is called
        func(result, offs);
        return true;
      }

      NBDASSERTEQUAL(opResult, nbdspv::DeviceOpResult::Failed);
      opResult = nbdspv::DeviceOpResult::Failed;
      return false;
    }
  }

  // Must be called from the replay manager thread (the debugger thread)
  void PopulateBuffer(uint64_t address, ShaderBindIndex bind)
  {
    CHECK_DEVICE_THREAD();
    // pick a non-overlapping bind namespace for direct pointer access
    ResourceId id;
    uint64_t ptrOffs;

    m_pDriver->GetResIDFromAddr(address, id, ptrOffs);
    if(id == ResourceId())
    {
      ShaderBindIndex noBind;
      CHECK_DEVICE_THREAD();
      auto insertIt = bufferCache.insert(std::make_pair(noBind, bytebuf()));
      m_pDriver->AddDebugMessage(MessageCategory::Execution, MessageSeverity::High,
                                 MessageSource::RuntimeWarning,
                                 StringFormat::Fmt("invalid or OOB pointer access detected ."));
      return;
    }

    // if the resources might be dirty from side-effects from the action, replay back to right
    // before it.
    if(m_ResourcesDirty)
    {
      VkMarkerRegion region("un-dirtying resources");
      m_pDriver->ReplayLog(0, m_EventID, eReplay_WithoutDraw);
      m_ResourcesDirty = false;
    }

    bytebuf data;
    m_pDriver->GetDebugManager()->GetBufferData(id, 0, 0, data);

    {
      // Insert atomically with all the data filled in : to prevent race conditions
      SCOPED_WRITELOCK(bufferCacheLock);
      auto insertIt = bufferCache.insert(std::make_pair(bind, data));
      NBDASSERT(insertIt.second);
    }
  }

  // Must be called from the replay manager thread (the debugger thread)
  void PopulateBuffer(const ShaderBindIndex &bind)
  {
    CHECK_DEVICE_THREAD();
    bytebuf data;

    bool valid = true;
    const Descriptor &bufData = GetDescriptor("accessing buffer value", bind, valid);
    if(valid)
    {
      // if the resources might be dirty from side-effects from the action, replay back to right
      // before it.
      if(m_ResourcesDirty)
      {
        VkMarkerRegion region("un-dirtying resources");
        m_pDriver->ReplayLog(0, m_EventID, eReplay_WithoutDraw);
        m_ResourcesDirty = false;
      }

      if(bufData.resource != ResourceId())
      {
        m_pDriver->GetReplay()->GetBufferData(bufData.resource, bufData.byteOffset,
                                              bufData.byteSize, data);
      }
    }

    {
      // Insert atomically with all the data filled in : to prevent race conditions
      SCOPED_WRITELOCK(bufferCacheLock);
      auto insertIt = bufferCache.insert(std::make_pair(bind, data));
      NBDASSERT(insertIt.second);
    }
  }

  // Must be called from the replay manager thread (the debugger thread)
  void PopulateImage(const ShaderBindIndex &bind)
  {
    CHECK_DEVICE_THREAD();

    ImageData data;
    bool valid = true;
    const Descriptor &imgData = GetDescriptor("performing image load/store", bind, valid);
    if(valid)
    {
      // if the resources might be dirty from side-effects from the action, replay back to right
      // before it.
      if(m_ResourcesDirty)
      {
        VkMarkerRegion region("un-dirtying resources");
        m_pDriver->ReplayLog(0, m_EventID, eReplay_WithoutDraw);
        m_ResourcesDirty = false;
      }

      if(imgData.type == DescriptorType::TypedBuffer ||
         imgData.type == DescriptorType::ReadWriteTypedBuffer)
      {
        VkFormat format;
        VkDeviceSize byteWidth;
        VkDeviceSize offset;
        ResourceId buffer;

        if(imgData.view == ResourceId())
        {
          // descriptor buffer, no buffer view
          buffer = imgData.resource;
          offset = imgData.byteOffset;
          format = MakeVkFormat(imgData.format);
          byteWidth = imgData.byteSize;
        }
        else
        {
          const VulkanCreationInfo::BufferView &viewProps =
              m_Creation.GetBufferViewInfo(imgData.view);
          buffer = viewProps.buffer;
          offset = viewProps.offset;
          format = viewProps.format;
          byteWidth = viewProps.size;
        }

        VulkanCreationInfo::Buffer defaultBufferProps = {};
        const VulkanCreationInfo::Buffer &bufferProps =
            (buffer == ResourceId()) ? defaultBufferProps : m_Creation.GetBufferInfo(buffer);

        // width in bytes, either from the view or from the remainder of the buffer
        if(byteWidth == VK_WHOLE_SIZE)
          byteWidth = bufferProps.size - offset;

        data.fmt = MakeResourceFormat(format);
        data.texelSize = (uint32_t)GetByteSize(1, 1, 1, format, 0);

        // convert to a texel width, rounding down as per spec (only possible from VK_WHOLE_SIZE)
        data.width = uint32_t(byteWidth / data.texelSize);
        data.height = 1;
        data.depth = 1;

        data.samplePitch = data.slicePitch = data.rowPitch = data.width * data.texelSize;

        m_pDriver->GetReplay()->GetBufferData(imgData.resource, offset, data.rowPitch, data.bytes);
      }
      else if(imgData.view != ResourceId())
      {
        const VulkanCreationInfo::ImageView &viewProps = m_Creation.GetImageViewInfo(imgData.view);
        if(viewProps.image != ResourceId())
        {
          const VulkanCreationInfo::Image &imageProps = m_Creation.GetImageInfo(viewProps.image);

          uint32_t mip = viewProps.range.baseMipLevel;

          data.width = NBDMAX(1U, imageProps.extent.width >> mip);
          data.height = NBDMAX(1U, imageProps.extent.height >> mip);
          if(imageProps.type == VK_IMAGE_TYPE_3D)
          {
            data.depth = NBDMAX(1U, imageProps.extent.depth >> mip);
          }
          else
          {
            data.depth = viewProps.range.layerCount;
            if(data.depth == VK_REMAINING_ARRAY_LAYERS)
              data.depth = imageProps.arrayLayers - viewProps.range.baseArrayLayer;
          }

          ResourceFormat fmt = MakeResourceFormat(imageProps.format);

          data.fmt = MakeResourceFormat(imageProps.format);
          data.texelSize = (uint32_t)GetByteSize(1, 1, 1, imageProps.format, 0);
          data.rowPitch = (uint32_t)GetByteSize(data.width, 1, 1, imageProps.format, 0);
          data.slicePitch = GetByteSize(data.width, data.height, 1, imageProps.format, 0);
          data.samplePitch = GetByteSize(data.width, data.height, data.depth, imageProps.format, 0);

          const uint32_t numSlices = imageProps.type == VK_IMAGE_TYPE_3D ? 1 : data.depth;
          const uint32_t numSamples = (uint32_t)imageProps.samples;

          data.bytes.reserve(size_t(data.samplePitch * numSamples));

          // defaults are fine - no interpretation. Maybe we could use the view's typecast?
          const GetTextureDataParams params = GetTextureDataParams();

          for(uint32_t sample = 0; sample < numSamples; sample++)
          {
            for(uint32_t slice = 0; slice < numSlices; slice++)
            {
              bytebuf subBytes;
              m_pDriver->GetReplay()->GetTextureData(
                  viewProps.image, Subresource(mip, slice, sample), params, subBytes);

              // fast path, swap into output if there's only one slice and one sample (common case)
              if(numSlices == 1 && numSamples == 1)
              {
                subBytes.swap(data.bytes);
              }
              else
              {
                data.bytes.append(subBytes);
              }
            }
          }
        }
      }
    }

    {
      // Insert atomically with all the data filled in : to prevent race conditions
      SCOPED_WRITELOCK(imageCacheLock);
      auto insertIt = imageCache.insert(std::make_pair(bind, data));
      NBDASSERT(insertIt.second);
    }
  }

  // Called from any thread
  bool IsImageCached(const ShaderBindIndex &bind) override
  {
    SCOPED_READLOCK(imageCacheLock);
    return imageCache.find(bind) != imageCache.end();
  }

  // Called from any thread
  ImageData *GetImageDataFromCache(const ShaderBindIndex &bind, nbdspv::DeviceOpResult &opResult)
  {
    // Calling function responsible for acquiring imageCache Read lock
    auto findIt = imageCache.find(bind);
    if(findIt != imageCache.end())
    {
      opResult = nbdspv::DeviceOpResult::Succeeded;
      return &findIt->second;
    }

    opResult = nbdspv::DeviceOpResult::Failed;

    // Not in the cache : populate must happen on the device thread
    if(!IsDeviceThread())
      opResult = nbdspv::DeviceOpResult::NeedsDevice;

    return NULL;
  }

  VkPipeline MakePipe(const ShaderConstParameters &params, uint32_t floatBitSize, bool depthTex,
                      bool uintTex, bool sintTex)
  {
    CHECK_DEVICE_THREAD();
    VkSpecializationMapEntry specMaps[sizeof(params) / sizeof(uint32_t)];
    for(size_t i = 0; i < ARRAY_COUNT(specMaps); i++)
    {
      specMaps[i].constantID = uint32_t(i);
      specMaps[i].offset = uint32_t(sizeof(uint32_t) * i);
      specMaps[i].size = sizeof(uint32_t);
    }

    VkSpecializationInfo specInfo = {};
    specInfo.dataSize = sizeof(params);
    specInfo.pData = &params;
    specInfo.mapEntryCount = ARRAY_COUNT(specMaps);
    specInfo.pMapEntries = specMaps;

    uint32_t shaderIndex = 0;
    if(depthTex)
      shaderIndex = 1;
    if(uintTex)
      shaderIndex = 2;
    else if(sintTex)
      shaderIndex = 3;

    if(params.operation == (uint32_t)nbdspv::Op::ExtInst)
    {
      shaderIndex = 4;
      if(floatBitSize == 16)
        shaderIndex = 5;
      else if(floatBitSize == 64)
        shaderIndex = 6;
    }

    if(m_DebugData.Module[shaderIndex] == VK_NULL_HANDLE)
    {
      nbdarray<uint32_t> spirv;

      if(params.operation == (uint32_t)nbdspv::Op::ExtInst)
      {
        GenerateMathShaderModule(spirv, floatBitSize);
      }
      else
      {
        NBDASSERTMSG("Assume sampling happens with 32-bit float inputs", floatBitSize == 32,
                     floatBitSize);
        GenerateSamplingShaderModule(spirv, depthTex, uintTex, sintTex);
      }

      VkShaderModuleCreateInfo moduleCreateInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      moduleCreateInfo.pCode = spirv.data();
      moduleCreateInfo.codeSize = spirv.size() * sizeof(uint32_t);

      VkResult vkr = m_pDriver->vkCreateShaderModule(m_pDriver->GetDev(), &moduleCreateInfo, NULL,
                                                     &m_DebugData.Module[shaderIndex]);
      CHECK_VKR(m_pDriver, vkr);

      const char *filename[] = {
          "/debug_psgather_float.spv", "/debug_psgather_depth.spv", "/debug_psgather_uint.spv",
          "/debug_psgather_sint.spv",  "/debug_psmath32.spv",       "/debug_psmath16.spv",
          "/debug_psmath64.spv",
      };

      if(!Vulkan_Debug_PSDebugDumpDirPath().empty())
        FileIO::WriteAll(Vulkan_Debug_PSDebugDumpDirPath() + filename[shaderIndex], spirv);
    }

    uint32_t key = params.hashKey(shaderIndex);

    if(m_DebugData.m_Pipelines[key] != VK_NULL_HANDLE)
      return m_DebugData.m_Pipelines[key];

    NBDLOG(
        "Making new pipeline for shader type %u, operation %s, dim %u, useGrad/useGatherOffsets %u,"
        " gather channel %u, gather offsets...",
        shaderIndex, ToStr((nbdspv::Op)params.operation).c_str(), params.dim,
        params.useGradOrGatherOffsets, params.gatherChannel);

    if(params.operation == (uint32_t)nbdspv::Op::ExtInst)
    {
      VkComputePipelineCreateInfo computePipeInfo = {
          VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          NULL,
          0,
          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
           VK_SHADER_STAGE_COMPUTE_BIT, m_DebugData.Module[shaderIndex], "main", NULL},
          m_DebugData.PipeLayout,
          VK_NULL_HANDLE,
          0,
      };

      VkPipeline pipe = VK_NULL_HANDLE;
      VkResult vkr = m_pDriver->vkCreateComputePipelines(m_pDriver->GetDev(),
                                                         m_pDriver->GetShaderCache()->GetPipeCache(),
                                                         1, &computePipeInfo, NULL, &pipe);
      if(vkr != VK_SUCCESS)
      {
        NBDERR("Failed creating debug pipeline: %s", ToStr(vkr).c_str());
        return VK_NULL_HANDLE;
      }

      m_DebugData.m_Pipelines[key] = pipe;

      return pipe;
    }

    const VkPipelineShaderStageCreateInfo shaderStages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT,
         m_pDriver->GetShaderCache()->GetBuiltinModule(BuiltinShader::ShaderDebugSampleVS), "main",
         NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         m_DebugData.Module[shaderIndex], "main", &specInfo},
    };

    const VkPipelineDynamicStateCreateInfo dynamicState = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    };

    const VkPipelineMultisampleStateCreateInfo msaa = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        NULL,
        0,
        VK_SAMPLE_COUNT_1_BIT,
    };

    const VkPipelineDepthStencilStateCreateInfo depthStencil = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };

    VkPipelineColorBlendAttachmentState colAttach = {};
    colAttach.colorWriteMask = 0xf;

    const VkPipelineColorBlendStateCreateInfo colorBlend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        NULL,
        0,
        false,
        VK_LOGIC_OP_NO_OP,
        1,
        &colAttach,
    };

    const VkPipelineVertexInputStateCreateInfo vertexInput = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    };

    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkRect2D s = {};
    s.extent.width = s.extent.height = 1;

    VkViewport v = {};
    v.width = v.height = v.maxDepth = 1.0f;

    VkPipelineViewportStateCreateInfo viewScissor = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    };
    viewScissor.viewportCount = viewScissor.scissorCount = 1;
    viewScissor.pScissors = &s;
    viewScissor.pViewports = &v;

    VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    };

    raster.lineWidth = 1.0f;

    const VkGraphicsPipelineCreateInfo graphicsPipeInfo = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        NULL,
        0,
        2,
        shaderStages,
        &vertexInput,
        &inputAssembly,
        NULL,    // tess
        &viewScissor,
        &raster,
        &msaa,
        &depthStencil,
        &colorBlend,
        &dynamicState,
        m_DebugData.PipeLayout,
        m_DebugData.RenderPass,
        0,                 // sub pass
        VK_NULL_HANDLE,    // base pipeline handle
        -1,                // base pipeline index
    };

    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult vkr = m_pDriver->vkCreateGraphicsPipelines(m_pDriver->GetDev(),
                                                        m_pDriver->GetShaderCache()->GetPipeCache(),
                                                        1, &graphicsPipeInfo, NULL, &pipe);
    if(vkr != VK_SUCCESS)
    {
      NBDERR("Failed creating debug pipeline: %s", ToStr(vkr).c_str());
      return VK_NULL_HANDLE;
    }

    m_DebugData.m_Pipelines[key] = pipe;

    return pipe;
  }

  void GenerateMathShaderModule(nbdarray<uint32_t> &spirv, uint32_t floatBitSize)
  {
    CHECK_DEVICE_THREAD();
    nbdspv::Editor editor(spirv);

    // create as SPIR-V 1.0 for best compatibility
    editor.CreateEmpty(1, 0);

    editor.AddCapability(nbdspv::Capability::Shader);

    nbdspv::Id entryId = editor.MakeId();

    editor.AddOperation(
        editor.Begin(nbdspv::Section::MemoryModel),
        nbdspv::OpMemoryModel(nbdspv::AddressingModel::Logical, nbdspv::MemoryModel::GLSL450));

    nbdspv::Id glsl450 = editor.ImportExtInst("GLSL.std.450");

    nbdspv::Scalar sizedScalar;
    if(floatBitSize == 32)
      sizedScalar = nbdspv::scalar<float>();
    else if(floatBitSize == 64)
      sizedScalar = nbdspv::scalar<double>();
    else
      sizedScalar = nbdspv::scalar<half_float::half>();

    nbdspv::Id u32 = editor.DeclareType(nbdspv::scalar<uint32_t>());
    nbdspv::Id floatType = editor.DeclareType(sizedScalar);
    nbdspv::Id vec4Type = editor.DeclareType(nbdspv::Vector(sizedScalar, 4));

    nbdspv::Id pushStructID =
        editor.AddType(nbdspv::OpTypeStruct(editor.MakeId(), {vec4Type, vec4Type, vec4Type, u32}));
    editor.AddDecoration(nbdspv::OpDecorate(pushStructID, nbdspv::Decoration::Block));
    editor.AddDecoration(nbdspv::OpMemberDecorate(
        pushStructID, 0, nbdspv::DecorationParam<nbdspv::Decoration::Offset>(0)));
    editor.AddDecoration(nbdspv::OpMemberDecorate(
        pushStructID, 1, nbdspv::DecorationParam<nbdspv::Decoration::Offset>(sizeof(Vec4f) * 2)));
    editor.AddDecoration(nbdspv::OpMemberDecorate(
        pushStructID, 2, nbdspv::DecorationParam<nbdspv::Decoration::Offset>(sizeof(Vec4f) * 4)));
    editor.AddDecoration(nbdspv::OpMemberDecorate(
        pushStructID, 3, nbdspv::DecorationParam<nbdspv::Decoration::Offset>(sizeof(Vec4f) * 6)));
    editor.SetMemberName(pushStructID, 0, "a");
    editor.SetMemberName(pushStructID, 1, "b");
    editor.SetMemberName(pushStructID, 2, "c");
    editor.SetMemberName(pushStructID, 3, "op");

    nbdspv::Id pushPtrType =
        editor.DeclareType(nbdspv::Pointer(pushStructID, nbdspv::StorageClass::PushConstant));
    nbdspv::Id pushVar = editor.AddVariable(
        nbdspv::OpVariable(pushPtrType, editor.MakeId(), nbdspv::StorageClass::PushConstant));
    editor.SetName(pushVar, "pushData");

    nbdspv::Id pushv4Type =
        editor.DeclareType(nbdspv::Pointer(vec4Type, nbdspv::StorageClass::PushConstant));
    nbdspv::Id pushu32Type =
        editor.DeclareType(nbdspv::Pointer(u32, nbdspv::StorageClass::PushConstant));

    nbdspv::Id storageStructType = editor.AddType(nbdspv::OpTypeStruct(editor.MakeId(), {vec4Type}));
    editor.AddDecoration(nbdspv::OpMemberDecorate(
        storageStructType, 0, nbdspv::DecorationParam<nbdspv::Decoration::Offset>(0)));
    editor.DecorateStorageBufferStruct(storageStructType);

    nbdspv::Id storageStructPtrType =
        editor.DeclareType(nbdspv::Pointer(storageStructType, editor.StorageBufferClass()));
    nbdspv::Id storageVec4PtrType =
        editor.DeclareType(nbdspv::Pointer(vec4Type, editor.StorageBufferClass()));

    nbdspv::Id storageVar = editor.AddVariable(
        nbdspv::OpVariable(storageStructPtrType, editor.MakeId(), editor.StorageBufferClass()));
    editor.AddDecoration(nbdspv::OpDecorate(
        storageVar, nbdspv::DecorationParam<nbdspv::Decoration::DescriptorSet>(0U)));
    editor.AddDecoration(nbdspv::OpDecorate(
        storageVar,
        nbdspv::DecorationParam<nbdspv::Decoration::Binding>((uint32_t)ShaderDebugBind::MathResult)));

    editor.SetName(storageVar, "resultStorage");

    // register the entry point
    editor.AddOperation(editor.Begin(nbdspv::Section::EntryPoints),
                        nbdspv::OpEntryPoint(nbdspv::ExecutionModel::GLCompute, entryId, "main", {}));
    editor.AddExecutionMode(nbdspv::OpExecutionMode(
        entryId, nbdspv::ExecutionModeParam<nbdspv::ExecutionMode::LocalSize>(1, 1, 1)));

    nbdspv::Id voidType = editor.DeclareType(nbdspv::scalar<void>());
    nbdspv::Id funcType = editor.DeclareType(nbdspv::FunctionType(voidType, {}));

    nbdspv::OperationList func;
    func.add(nbdspv::OpFunction(voidType, entryId, nbdspv::FunctionControl::None, funcType));
    func.add(nbdspv::OpLabel(editor.MakeId()));

    nbdspv::Id consts[] = {
        editor.AddConstantImmediate<uint32_t>(0),
        editor.AddConstantImmediate<uint32_t>(1),
        editor.AddConstantImmediate<uint32_t>(2),
        editor.AddConstantImmediate<uint32_t>(3),
    };

    nbdspv::Id zerof;
    if(floatBitSize == 32)
      zerof = editor.AddConstantImmediate<float>(0.0f);
    else if(floatBitSize == 64)
      zerof = editor.AddConstantImmediate<double>(0.0);
    else
      zerof = editor.AddConstantImmediate<half_float::half>(half_float::half(0.0f));

    // load the parameters and the op
    nbdspv::Id aPtr =
        func.add(nbdspv::OpAccessChain(pushv4Type, editor.MakeId(), pushVar, {consts[0]}));
    nbdspv::Id bPtr =
        func.add(nbdspv::OpAccessChain(pushv4Type, editor.MakeId(), pushVar, {consts[1]}));
    nbdspv::Id cPtr =
        func.add(nbdspv::OpAccessChain(pushv4Type, editor.MakeId(), pushVar, {consts[2]}));
    nbdspv::Id opPtr =
        func.add(nbdspv::OpAccessChain(pushu32Type, editor.MakeId(), pushVar, {consts[3]}));
    nbdspv::Id a = func.add(nbdspv::OpLoad(vec4Type, editor.MakeId(), aPtr));
    nbdspv::Id b = func.add(nbdspv::OpLoad(vec4Type, editor.MakeId(), bPtr));
    nbdspv::Id c = func.add(nbdspv::OpLoad(vec4Type, editor.MakeId(), cPtr));
    nbdspv::Id opParam = func.add(nbdspv::OpLoad(u32, editor.MakeId(), opPtr));

    // access chain the output
    nbdspv::Id outVar =
        func.add(nbdspv::OpAccessChain(storageVec4PtrType, editor.MakeId(), storageVar, {consts[0]}));

    nbdspv::Id breakLabel = editor.MakeId();
    nbdspv::Id defaultLabel = editor.MakeId();

    nbdarray<nbdspv::SwitchPairU32LiteralId> targets;

    nbdspv::OperationList cases;

    // all these operations take one parameter and only operate on floats (possibly vectors)
    for(nbdspv::GLSLstd450 op : {
            nbdspv::GLSLstd450::Sin,       nbdspv::GLSLstd450::Cos,
            nbdspv::GLSLstd450::Tan,       nbdspv::GLSLstd450::Asin,
            nbdspv::GLSLstd450::Acos,      nbdspv::GLSLstd450::Atan,
            nbdspv::GLSLstd450::Sinh,      nbdspv::GLSLstd450::Cosh,
            nbdspv::GLSLstd450::Tanh,      nbdspv::GLSLstd450::Asinh,
            nbdspv::GLSLstd450::Acosh,     nbdspv::GLSLstd450::Atanh,
            nbdspv::GLSLstd450::Exp,       nbdspv::GLSLstd450::Log,
            nbdspv::GLSLstd450::Exp2,      nbdspv::GLSLstd450::Log2,
            nbdspv::GLSLstd450::Sqrt,      nbdspv::GLSLstd450::InverseSqrt,
            nbdspv::GLSLstd450::Normalize,
        })
    {
      // most operations aren't allowed on doubles
      if(floatBitSize == 64 && op != nbdspv::GLSLstd450::Sqrt &&
         op != nbdspv::GLSLstd450::InverseSqrt && op != nbdspv::GLSLstd450::Normalize)
        continue;

      nbdspv::Id label = editor.MakeId();
      targets.push_back({(uint32_t)op, label});

      cases.add(nbdspv::OpLabel(label));
      nbdspv::Id result = cases.add(nbdspv::OpGLSL450(vec4Type, editor.MakeId(), glsl450, op, {a}));
      cases.add(nbdspv::OpStore(outVar, result));
      cases.add(nbdspv::OpBranch(breakLabel));
    }

    if(floatBitSize != 64)
    {
      // these take two parameters, but are otherwise identical
      for(nbdspv::GLSLstd450 op : {nbdspv::GLSLstd450::Atan2, nbdspv::GLSLstd450::Pow})
      {
        nbdspv::Id label = editor.MakeId();
        targets.push_back({(uint32_t)op, label});

        cases.add(nbdspv::OpLabel(label));
        nbdspv::Id result =
            cases.add(nbdspv::OpGLSL450(vec4Type, editor.MakeId(), glsl450, op, {a, b}));
        cases.add(nbdspv::OpStore(outVar, result));
        cases.add(nbdspv::OpBranch(breakLabel));
      }
    }

    {
      nbdspv::GLSLstd450 op = nbdspv::GLSLstd450::Fma;

      nbdspv::Id label = editor.MakeId();
      targets.push_back({(uint32_t)op, label});

      cases.add(nbdspv::OpLabel(label));
      nbdspv::Id result =
          cases.add(nbdspv::OpGLSL450(vec4Type, editor.MakeId(), glsl450, op, {a, b, c}));
      cases.add(nbdspv::OpStore(outVar, result));
      cases.add(nbdspv::OpBranch(breakLabel));
    }

    // these ones are special
    {
      nbdspv::GLSLstd450 op = nbdspv::GLSLstd450::Length;

      nbdspv::Id label = editor.MakeId();
      targets.push_back({(uint32_t)op, label});

      cases.add(nbdspv::OpLabel(label));
      nbdspv::Id result = cases.add(nbdspv::OpGLSL450(floatType, editor.MakeId(), glsl450, op, {a}));
      nbdspv::Id resultvec = cases.add(
          nbdspv::OpCompositeConstruct(vec4Type, editor.MakeId(), {result, zerof, zerof, zerof}));
      cases.add(nbdspv::OpStore(outVar, resultvec));
      cases.add(nbdspv::OpBranch(breakLabel));
    }

    {
      nbdspv::GLSLstd450 op = nbdspv::GLSLstd450::Distance;

      nbdspv::Id label = editor.MakeId();
      targets.push_back({(uint32_t)op, label});

      cases.add(nbdspv::OpLabel(label));
      nbdspv::Id result =
          cases.add(nbdspv::OpGLSL450(floatType, editor.MakeId(), glsl450, op, {a, b}));
      nbdspv::Id resultvec = cases.add(
          nbdspv::OpCompositeConstruct(vec4Type, editor.MakeId(), {result, zerof, zerof, zerof}));
      cases.add(nbdspv::OpStore(outVar, resultvec));
      cases.add(nbdspv::OpBranch(breakLabel));
    }

    {
      nbdspv::GLSLstd450 op = nbdspv::GLSLstd450::Refract;

      nbdspv::Id label = editor.MakeId();
      targets.push_back({(uint32_t)op, label});

      cases.add(nbdspv::OpLabel(label));
      nbdspv::Id eta = cases.add(nbdspv::OpCompositeExtract(floatType, editor.MakeId(), c, {0}));
      nbdspv::Id result =
          cases.add(nbdspv::OpGLSL450(vec4Type, editor.MakeId(), glsl450, op, {a, b, eta}));
      cases.add(nbdspv::OpStore(outVar, result));
      cases.add(nbdspv::OpBranch(breakLabel));
    }

    // non-glsl opcodes
    if(m_pDriver->PreciseFMAMask() & floatBitSize)
    {
      editor.AddCapability(nbdspv::Capability::FMAKHR);

      uint32_t op = 1000 + (uint32_t)nbdspv::Op::FmaKHR;

      nbdspv::Id label = editor.MakeId();
      targets.push_back({(uint32_t)op, label});

      cases.add(nbdspv::OpLabel(label));
      nbdspv::Id result = cases.add(nbdspv::OpFmaKHR(vec4Type, editor.MakeId(), a, b, c));
      cases.add(nbdspv::OpStore(outVar, result));
      cases.add(nbdspv::OpBranch(breakLabel));
    }

    func.add(nbdspv::OpSelectionMerge(breakLabel, nbdspv::SelectionControl::None));
    func.add(nbdspv::OpSwitch32(opParam, defaultLabel, targets));

    func.append(cases);

    // default: store NULL data
    func.add(nbdspv::OpLabel(defaultLabel));
    func.add(nbdspv::OpStore(
        outVar, editor.AddConstant(nbdspv::OpConstantNull(vec4Type, editor.MakeId()))));
    func.add(nbdspv::OpBranch(breakLabel));

    func.add(nbdspv::OpLabel(breakLabel));
    func.add(nbdspv::OpReturn());
    func.add(nbdspv::OpFunctionEnd());

    editor.AddFunction(func);
  }

  void GenerateSamplingShaderModule(nbdarray<uint32_t> &spirv, bool depthTex, bool uintTex,
                                    bool sintTex)
  {
    CHECK_DEVICE_THREAD();
    // this could be done as a glsl shader, but glslang has some bugs compiling the specialisation
    // constants, so we generate it by hand - which isn't too hard

    nbdspv::Editor editor(spirv);

    // create as SPIR-V 1.0 for best compatibility
    editor.CreateEmpty(1, 0);

    editor.AddCapability(nbdspv::Capability::Shader);
    editor.AddCapability(nbdspv::Capability::ImageQuery);
    editor.AddCapability(nbdspv::Capability::Sampled1D);
    editor.AddCapability(nbdspv::Capability::SampledBuffer);

    if(m_pDriver->GetDeviceEnabledFeatures().shaderResourceMinLod)
      editor.AddCapability(nbdspv::Capability::MinLod);
    if(m_pDriver->GetDeviceEnabledFeatures().shaderImageGatherExtended)
      editor.AddCapability(nbdspv::Capability::ImageGatherExtended);

    const bool cubeArray = (m_pDriver->GetDeviceEnabledFeatures().imageCubeArray != VK_FALSE);

    nbdspv::Id entryId = editor.MakeId();

    editor.AddOperation(
        editor.Begin(nbdspv::Section::MemoryModel),
        nbdspv::OpMemoryModel(nbdspv::AddressingModel::Logical, nbdspv::MemoryModel::GLSL450));

    nbdspv::Id u32 = editor.DeclareType(nbdspv::scalar<uint32_t>());
    nbdspv::Id i32 = editor.DeclareType(nbdspv::scalar<int32_t>());
    nbdspv::Id f32 = editor.DeclareType(nbdspv::scalar<float>());

    nbdspv::Id v2i32 = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<int32_t>(), 2));
    nbdspv::Id v3i32 = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<int32_t>(), 3));
    nbdspv::Id v2f32 = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), 2));
    nbdspv::Id v3f32 = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), 3));
    nbdspv::Id v4f32 = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), 4));

    // int2[4]
    nbdspv::Id a4v2i32 = editor.AddType(
        nbdspv::OpTypeArray(editor.MakeId(), v2i32, editor.AddConstantImmediate<uint32_t>(4)));

    nbdspv::Scalar base = nbdspv::scalar<float>();
    if(uintTex)
      base = nbdspv::scalar<uint32_t>();
    else if(sintTex)
      base = nbdspv::scalar<int32_t>();

    nbdspv::Id resultType = editor.DeclareType(nbdspv::Vector(base, 4));
    nbdspv::Id scalarResultType = editor.DeclareType(base);

// add specialisation constants for all the parameters
#define MEMBER_IDX(struct, name) uint32_t(offsetof(struct, name) / sizeof(uint32_t))

#define DECL_SPECID(type, name, value)                                                     \
  nbdspv::Id name =                                                                        \
      editor.AddSpecConstantImmediate<type>(0U, MEMBER_IDX(ShaderConstParameters, value)); \
  editor.SetName(name, "spec_" #name);

    DECL_SPECID(uint32_t, operation, operation);
    DECL_SPECID(bool, useGradOrGatherOffsets, useGradOrGatherOffsets);
    DECL_SPECID(uint32_t, dim, dim);
    DECL_SPECID(int32_t, gatherChannel, gatherChannel);
    DECL_SPECID(int32_t, gather_u0, gatherOffsets.u0);
    DECL_SPECID(int32_t, gather_v0, gatherOffsets.v0);
    DECL_SPECID(int32_t, gather_u1, gatherOffsets.u1);
    DECL_SPECID(int32_t, gather_v1, gatherOffsets.v1);
    DECL_SPECID(int32_t, gather_u2, gatherOffsets.u2);
    DECL_SPECID(int32_t, gather_v2, gatherOffsets.v2);
    DECL_SPECID(int32_t, gather_u3, gatherOffsets.u3);
    DECL_SPECID(int32_t, gather_v3, gatherOffsets.v3);

    struct StructMember
    {
      nbdspv::Id loadedType;
      nbdspv::Id ptrType;
      nbdspv::Id loadedId;
      const char *name;
      uint32_t memberIndex;
    };

    nbdarray<nbdspv::Id> memberIds;
    nbdarray<StructMember> cbufferMembers;

    nbdspv::Id type_int32_t = editor.DeclareType(nbdspv::scalar<int32_t>());
    nbdspv::Id type_float = editor.DeclareType(nbdspv::scalar<float>());

    nbdspv::Id uniformptr_int32_t =
        editor.DeclareType(nbdspv::Pointer(type_int32_t, nbdspv::StorageClass::Uniform));
    nbdspv::Id uniformptr_float =
        editor.DeclareType(nbdspv::Pointer(type_float, nbdspv::StorageClass::Uniform));

#define DECL_UNIFORM(type, name, value)                                                  \
  nbdspv::Id name = editor.MakeId();                                                     \
  editor.SetName(name, "uniform_" #name);                                                \
  cbufferMembers.push_back({CONCAT(type_, type), CONCAT(uniformptr_, type), name, #name, \
                            MEMBER_IDX(ShaderUniformParameters, value)});                \
  memberIds.push_back(CONCAT(type_, type));
    DECL_UNIFORM(int32_t, texel_u, texel_uvw.x);
    DECL_UNIFORM(int32_t, texel_v, texel_uvw.y);
    DECL_UNIFORM(int32_t, texel_w, texel_uvw.z);
    DECL_UNIFORM(int32_t, texel_lod, texel_lod);
    DECL_UNIFORM(float, u, uvwa[0]);
    DECL_UNIFORM(float, v, uvwa[1]);
    DECL_UNIFORM(float, w, uvwa[2]);
    DECL_UNIFORM(float, cube_a, uvwa[3]);
    DECL_UNIFORM(float, dudx, ddx[0]);
    DECL_UNIFORM(float, dvdx, ddx[1]);
    DECL_UNIFORM(float, dwdx, ddx[2]);
    DECL_UNIFORM(float, dudy, ddy[0]);
    DECL_UNIFORM(float, dvdy, ddy[1]);
    DECL_UNIFORM(float, dwdy, ddy[2]);
    DECL_UNIFORM(int32_t, dynoffset_u, offset.x);
    DECL_UNIFORM(int32_t, dynoffset_v, offset.y);
    DECL_UNIFORM(int32_t, dynoffset_w, offset.z);
    DECL_UNIFORM(int32_t, sampleIdx, sampleIdx);
    DECL_UNIFORM(float, compare, compare);
    DECL_UNIFORM(float, lod, lod);
    DECL_UNIFORM(float, minlod, minlod);

    nbdspv::Id cbufferStructID = editor.AddType(nbdspv::OpTypeStruct(editor.MakeId(), memberIds));
    editor.AddDecoration(nbdspv::OpDecorate(cbufferStructID, nbdspv::Decoration::Block));
    for(const StructMember &m : cbufferMembers)
    {
      editor.AddDecoration(nbdspv::OpMemberDecorate(
          cbufferStructID, m.memberIndex,
          nbdspv::DecorationParam<nbdspv::Decoration::Offset>(m.memberIndex * sizeof(uint32_t))));
      editor.SetMemberName(cbufferStructID, m.memberIndex, m.name);
    }

    nbdspv::Id constoffset_u = gather_u0;
    nbdspv::Id constoffset_uv = editor.AddConstant(
        nbdspv::OpSpecConstantComposite(v2i32, editor.MakeId(), {gather_u0, gather_v0}));
    nbdspv::Id constoffset_uvw = editor.AddConstant(
        nbdspv::OpSpecConstantComposite(v3i32, editor.MakeId(), {gather_u0, gather_v0, gather_u1}));
    editor.SetName(constoffset_u, "constoffset_u");
    editor.SetName(constoffset_uv, "constoffset_uv");
    editor.SetName(constoffset_uvw, "constoffset_uvw");

    nbdspv::Id gather_0 = editor.AddConstant(
        nbdspv::OpSpecConstantComposite(v2i32, editor.MakeId(), {gather_u0, gather_v0}));
    nbdspv::Id gather_1 = editor.AddConstant(
        nbdspv::OpSpecConstantComposite(v2i32, editor.MakeId(), {gather_u1, gather_v1}));
    nbdspv::Id gather_2 = editor.AddConstant(
        nbdspv::OpSpecConstantComposite(v2i32, editor.MakeId(), {gather_u2, gather_v2}));
    nbdspv::Id gather_3 = editor.AddConstant(
        nbdspv::OpSpecConstantComposite(v2i32, editor.MakeId(), {gather_u3, gather_v3}));

    nbdspv::Id gatherOffsets = editor.AddConstant(nbdspv::OpSpecConstantComposite(
        a4v2i32, editor.MakeId(), {gather_0, gather_1, gather_2, gather_3}));

    editor.SetName(gatherOffsets, "gatherOffsets");

    // create the output. It's always a 4-wide vector
    nbdspv::Id outPtrType =
        editor.DeclareType(nbdspv::Pointer(resultType, nbdspv::StorageClass::Output));

    nbdspv::Id outVar = editor.AddVariable(
        nbdspv::OpVariable(outPtrType, editor.MakeId(), nbdspv::StorageClass::Output));
    editor.AddDecoration(
        nbdspv::OpDecorate(outVar, nbdspv::DecorationParam<nbdspv::Decoration::Location>(0)));

    editor.SetName(outVar, "output");

    nbdspv::ImageFormat unk = nbdspv::ImageFormat::Unknown;

    // create the five textures and sampler
    nbdspv::Id texSampTypes[(uint32_t)ShaderDebugBind::Count] = {
        nbdspv::Id(),
        editor.DeclareType(nbdspv::Image(base, nbdspv::Dim::_1D, 0, 1, 0, 1, unk)),
        editor.DeclareType(nbdspv::Image(base, nbdspv::Dim::_2D, 0, 1, 0, 1, unk)),
        editor.DeclareType(nbdspv::Image(base, nbdspv::Dim::_3D, 0, 0, 0, 1, unk)),
        editor.DeclareType(nbdspv::Image(base, nbdspv::Dim::_2D, 0, 1, 1, 1, unk)),
        editor.DeclareType(nbdspv::Image(base, nbdspv::Dim::Cube, 0, cubeArray ? 1 : 0, 0, 1, unk)),
        editor.DeclareType(nbdspv::Image(base, nbdspv::Dim::Buffer, 0, 0, 0, 1, unk)),
        editor.DeclareType(nbdspv::Sampler()),
        cbufferStructID,
    };
    nbdspv::Id bindVars[(uint32_t)ShaderDebugBind::Count];
    nbdspv::Id texSampCombinedTypes[(uint32_t)ShaderDebugBind::Count] = {
        nbdspv::Id(),
        editor.DeclareType(nbdspv::SampledImage(texSampTypes[1])),
        editor.DeclareType(nbdspv::SampledImage(texSampTypes[2])),
        editor.DeclareType(nbdspv::SampledImage(texSampTypes[3])),
        editor.DeclareType(nbdspv::SampledImage(texSampTypes[4])),
        editor.DeclareType(nbdspv::SampledImage(texSampTypes[5])),
        editor.DeclareType(nbdspv::SampledImage(texSampTypes[6])),
        nbdspv::Id(),
        nbdspv::Id(),
    };

    for(size_t i = (size_t)ShaderDebugBind::First; i < (size_t)ShaderDebugBind::Count; i++)
    {
      nbdspv::StorageClass storageClass = nbdspv::StorageClass::UniformConstant;

      if(depthTex)
      {
        if(i == (size_t)ShaderDebugBind::Tex3D && !m_pDriver->GetReplay()->Depth3DSupported())
          continue;
        else if(i == (size_t)ShaderDebugBind::TexCube && !m_pDriver->GetReplay()->DepthCubeSupported())
          continue;
      }

      if(i == (size_t)ShaderDebugBind::Constants)
        storageClass = nbdspv::StorageClass::Uniform;

      nbdspv::Id ptrType = editor.DeclareType(nbdspv::Pointer(texSampTypes[i], storageClass));

      bindVars[i] = editor.AddVariable(nbdspv::OpVariable(ptrType, editor.MakeId(), storageClass));

      editor.AddDecoration(nbdspv::OpDecorate(
          bindVars[i], nbdspv::DecorationParam<nbdspv::Decoration::DescriptorSet>(0U)));
      editor.AddDecoration(nbdspv::OpDecorate(
          bindVars[i], nbdspv::DecorationParam<nbdspv::Decoration::Binding>((uint32_t)i)));
    }

    editor.SetName(bindVars[(size_t)ShaderDebugBind::Tex1D], "Tex1D");
    editor.SetName(bindVars[(size_t)ShaderDebugBind::Tex2D], "Tex2D");
    if(bindVars[(size_t)ShaderDebugBind::Tex3D] != nbdspv::Id())
      editor.SetName(bindVars[(size_t)ShaderDebugBind::Tex3D], "Tex3D");
    editor.SetName(bindVars[(size_t)ShaderDebugBind::Tex2DMS], "Tex2DMS");
    if(bindVars[(size_t)ShaderDebugBind::TexCube] != nbdspv::Id())
      editor.SetName(bindVars[(size_t)ShaderDebugBind::TexCube], "TexCube");
    editor.SetName(bindVars[(size_t)ShaderDebugBind::Buffer], "Buffer");
    editor.SetName(bindVars[(size_t)ShaderDebugBind::Sampler], "Sampler");
    editor.SetName(bindVars[(size_t)ShaderDebugBind::Constants], "CBuffer");

    nbdspv::Id uvwa_ptr = editor.DeclareType(nbdspv::Pointer(v4f32, nbdspv::StorageClass::Input));
    nbdspv::Id input_uvwa_var = editor.AddVariable(
        nbdspv::OpVariable(uvwa_ptr, editor.MakeId(), nbdspv::StorageClass::Input));
    editor.AddDecoration(nbdspv::OpDecorate(
        input_uvwa_var, nbdspv::DecorationParam<nbdspv::Decoration::Location>(0)));

    editor.SetName(input_uvwa_var, "input_uvwa");

    // register the entry point
    editor.AddOperation(editor.Begin(nbdspv::Section::EntryPoints),
                        nbdspv::OpEntryPoint(nbdspv::ExecutionModel::Fragment, entryId, "main",
                                             {input_uvwa_var, outVar}));
    editor.AddExecutionMode(nbdspv::OpExecutionMode(entryId, nbdspv::ExecutionMode::OriginUpperLeft));

    nbdspv::Id voidType = editor.DeclareType(nbdspv::scalar<void>());
    nbdspv::Id funcType = editor.DeclareType(nbdspv::FunctionType(voidType, {}));

    nbdspv::OperationList func;
    func.add(nbdspv::OpFunction(voidType, entryId, nbdspv::FunctionControl::None, funcType));
    func.add(nbdspv::OpLabel(editor.MakeId()));

    // access chain and load all the cbuffer variables
    for(const StructMember &m : cbufferMembers)
    {
      nbdspv::Id ptr = func.add(nbdspv::OpAccessChain(
          m.ptrType, editor.MakeId(), bindVars[(size_t)ShaderDebugBind::Constants],
          {editor.AddConstantImmediate<uint32_t>(m.memberIndex)}));
      func.add(nbdspv::OpLoad(m.loadedType, m.loadedId, ptr));
    }

    // declare cbuffer composites
    nbdspv::Id texel_uv =
        func.add(nbdspv::OpCompositeConstruct(v2i32, editor.MakeId(), {texel_u, texel_v}));
    nbdspv::Id texel_uvw =
        func.add(nbdspv::OpCompositeConstruct(v3i32, editor.MakeId(), {texel_u, texel_v, texel_w}));

    editor.SetName(texel_uv, "texel_uv");
    editor.SetName(texel_uvw, "texel_uvw");

    nbdspv::Id uv = func.add(nbdspv::OpCompositeConstruct(v2f32, editor.MakeId(), {u, v}));
    nbdspv::Id uvw = func.add(nbdspv::OpCompositeConstruct(v3f32, editor.MakeId(), {u, v, w}));
    nbdspv::Id uvwa =
        func.add(nbdspv::OpCompositeConstruct(v4f32, editor.MakeId(), {u, v, w, cube_a}));

    editor.SetName(uv, "uv");
    editor.SetName(uvw, "uvw");
    editor.SetName(uvwa, "uvwa");

    nbdspv::Id ddx_uv = func.add(nbdspv::OpCompositeConstruct(v2f32, editor.MakeId(), {dudx, dvdx}));
    nbdspv::Id ddx_uvw =
        func.add(nbdspv::OpCompositeConstruct(v3f32, editor.MakeId(), {dudx, dvdx, dwdx}));

    editor.SetName(ddx_uv, "ddx_uv");
    editor.SetName(ddx_uvw, "ddx_uvw");

    nbdspv::Id ddy_uv = func.add(nbdspv::OpCompositeConstruct(v2f32, editor.MakeId(), {dudy, dvdy}));
    nbdspv::Id ddy_uvw =
        func.add(nbdspv::OpCompositeConstruct(v3f32, editor.MakeId(), {dudy, dvdy, dwdy}));

    editor.SetName(ddy_uv, "ddy_uv");
    editor.SetName(ddy_uvw, "ddy_uvw");

    nbdspv::Id dynoffset_uv =
        func.add(nbdspv::OpCompositeConstruct(v2i32, editor.MakeId(), {dynoffset_u, dynoffset_v}));
    nbdspv::Id dynoffset_uvw = func.add(nbdspv::OpCompositeConstruct(
        v3i32, editor.MakeId(), {dynoffset_u, dynoffset_v, dynoffset_w}));

    editor.SetName(dynoffset_uv, "dynoffset_uv");
    editor.SetName(dynoffset_uvw, "dynoffset_uvw");

    nbdspv::Id input_uvwa = func.add(nbdspv::OpLoad(v4f32, editor.MakeId(), input_uvwa_var));
    nbdspv::Id input_uvw =
        func.add(nbdspv::OpVectorShuffle(v3f32, editor.MakeId(), input_uvwa, input_uvwa, {0, 1, 2}));
    nbdspv::Id input_uv =
        func.add(nbdspv::OpVectorShuffle(v2f32, editor.MakeId(), input_uvw, input_uvw, {0, 1}));
    nbdspv::Id input_u = func.add(nbdspv::OpCompositeExtract(f32, editor.MakeId(), input_uvw, {0}));

    // first store NULL data in, so the output is always initialised

    nbdspv::Id breakLabel = editor.MakeId();
    nbdspv::Id defaultLabel = editor.MakeId();

    // combine the operation with the image type:
    // operation * 10 + dim
    NBDCOMPILE_ASSERT(size_t(ShaderDebugBind::Count) < 10, "Combining value ranges will overlap!");
    nbdspv::Id switchVal = func.add(nbdspv::OpIMul(u32, editor.MakeId(), operation,
                                                   editor.AddConstantImmediate<uint32_t>(10U)));
    switchVal = func.add(nbdspv::OpIAdd(u32, editor.MakeId(), switchVal, dim));

    // switch on the combined operation and image type value
    nbdarray<nbdspv::SwitchPairU32LiteralId> targets;

    nbdspv::OperationList cases;

    nbdspv::Id texel_coord[(uint32_t)ShaderDebugBind::Count] = {
        nbdspv::Id(),
        texel_uv,        // 1D - u and array
        texel_uvw,       // 2D - u,v and array
        texel_uvw,       // 3D - u,v,w
        texel_uvw,       // 2DMS - u,v and array
        nbdspv::Id(),    // Cube
        texel_u,         // Buffer - u
    };

    // only used for QueryLod, so we can ignore MSAA/Buffer
    nbdspv::Id input_coord[(uint32_t)ShaderDebugBind::Count] = {
        nbdspv::Id(),
        input_u,         // 1D - u
        input_uv,        // 2D - u,v
        input_uvw,       // 3D - u,v,w
        nbdspv::Id(),    // 2DMS
        input_uvw,       // Cube - u,v,w
        nbdspv::Id(),    // Buffer
    };

    nbdspv::Id coord[(uint32_t)ShaderDebugBind::Count] = {
        nbdspv::Id(),
        uv,                        // 1D - u and array
        uvw,                       // 2D - u,v and array
        uvw,                       // 3D - u,v,w
        uvw,                       // 2DMS - u,v and array
        cubeArray ? uvwa : uvw,    // Cube - u,v,w and array (if supported)
        u,                         // Buffer - u
    };

    nbdspv::Id constoffset[(uint32_t)ShaderDebugBind::Count] = {
        nbdspv::Id(),
        constoffset_u,      // 1D - u
        constoffset_uv,     // 2D - u,v
        constoffset_uvw,    // 3D - u,v,w
        constoffset_uv,     // 2DMS - u,v
        nbdspv::Id(),       // Cube - not valid
        constoffset_u,      // Buffer - u
    };

    nbdspv::Id dynoffset[(uint32_t)ShaderDebugBind::Count] = {
        nbdspv::Id(),
        dynoffset_u,      // 1D - u
        dynoffset_uv,     // 2D - u,v
        dynoffset_uvw,    // 3D - u,v,w
        dynoffset_uv,     // 2DMS - u,v
        nbdspv::Id(),     // Cube - not valid
        dynoffset_u,      // Buffer - u
    };

    nbdspv::Id ddxs[(uint32_t)ShaderDebugBind::Count] = {
        nbdspv::Id(),
        dudx,       // 1D - u
        ddx_uv,     // 2D - u,v
        ddx_uvw,    // 3D - u,v,w
        ddx_uv,     // 2DMS - u,v
        ddx_uvw,    // Cube - u,v,w
        dudx,       // Buffer - u
    };

    nbdspv::Id ddys[(uint32_t)ShaderDebugBind::Count] = {
        nbdspv::Id(),
        dudy,       // 1D - u
        ddy_uv,     // 2D - u,v
        ddy_uvw,    // 3D - u,v,w
        ddy_uv,     // 2DMS - u,v
        ddy_uvw,    // Cube - u,v,w
        dudy,       // Buffer - u
    };

    uint32_t sampIdx = (uint32_t)ShaderDebugBind::Sampler;

    nbdspv::Id zerof = editor.AddConstantImmediate<float>(0.0f);

    for(uint32_t i = (uint32_t)ShaderDebugBind::First; i < (uint32_t)ShaderDebugBind::Count; i++)
    {
      if(i == sampIdx || i == (uint32_t)ShaderDebugBind::Constants)
        continue;

      if(bindVars[i] == nbdspv::Id())
        continue;

      nbdspv::ImageOperandsAndParamDatas imageOperandsWithOffsets;

      // most operations support offsets, set the operands commonly here.
      // with the shaderImageGatherExtended feature, gather opcodes will always get their operands
      // via uniforms to cut down on pipeline specialisations a little, but all other cases the
      // offsets must be constant.
      if(constoffset[i] != nbdspv::Id())
        imageOperandsWithOffsets.setConstOffset(constoffset[i]);

      // can't fetch from cubemaps
      if(i != (uint32_t)ShaderDebugBind::TexCube)
      {
        nbdspv::Op op = nbdspv::Op::ImageFetch;

        nbdspv::Id label = editor.MakeId();
        targets.push_back({(uint32_t)op * 10 + i, label});

        nbdspv::ImageOperandsAndParamDatas operands = imageOperandsWithOffsets;

        if(i != (uint32_t)ShaderDebugBind::Buffer && i != (uint32_t)ShaderDebugBind::Tex2DMS)
          operands.setLod(texel_lod);

        if(i == (uint32_t)ShaderDebugBind::Tex2DMS)
          operands.setSample(sampleIdx);

        cases.add(nbdspv::OpLabel(label));
        nbdspv::Id loaded = cases.add(nbdspv::OpLoad(texSampTypes[i], editor.MakeId(), bindVars[i]));
        nbdspv::Id sampleResult = cases.add(
            nbdspv::OpImageFetch(resultType, editor.MakeId(), loaded, texel_coord[i], operands));
        cases.add(nbdspv::OpStore(outVar, sampleResult));
        cases.add(nbdspv::OpBranch(breakLabel));
      }

      // buffers and multisampled images don't support sampling, so skip the other operations at
      // this point
      if(i == (uint32_t)ShaderDebugBind::Buffer || i == (uint32_t)ShaderDebugBind::Tex2DMS)
        continue;

      {
        nbdspv::Op op = nbdspv::Op::ImageQueryLod;

        nbdspv::Id label = editor.MakeId();
        targets.push_back({(uint32_t)op * 10 + i, label});

        cases.add(nbdspv::OpLabel(label));
        nbdspv::Id loadedImage =
            cases.add(nbdspv::OpLoad(texSampTypes[i], editor.MakeId(), bindVars[i]));
        nbdspv::Id loadedSampler =
            cases.add(nbdspv::OpLoad(texSampTypes[sampIdx], editor.MakeId(), bindVars[sampIdx]));

        nbdspv::Id combined = cases.add(nbdspv::OpSampledImage(
            texSampCombinedTypes[i], editor.MakeId(), loadedImage, loadedSampler));

        nbdspv::Id sampleResult =
            cases.add(nbdspv::OpImageQueryLod(v2f32, editor.MakeId(), combined, input_coord[i]));
        sampleResult = cases.add(nbdspv::OpVectorShuffle(v4f32, editor.MakeId(), sampleResult,
                                                         sampleResult, {0, 1, 0, 1}));

        // if we're sampling from an integer texture the output variable will be the same type.
        // Just bitcast the float bits into it, which will come out the other side the right type.
        if(uintTex || sintTex)
          sampleResult = cases.add(nbdspv::OpBitcast(resultType, editor.MakeId(), sampleResult));

        cases.add(nbdspv::OpStore(outVar, sampleResult));
        cases.add(nbdspv::OpBranch(breakLabel));
      }

      for(nbdspv::Op op : {nbdspv::Op::ImageSampleExplicitLod, nbdspv::Op::ImageSampleImplicitLod})
      {
        nbdspv::Id label = editor.MakeId();
        targets.push_back({(uint32_t)op * 10 + i, label});

        cases.add(nbdspv::OpLabel(label));
        nbdspv::Id loadedImage =
            cases.add(nbdspv::OpLoad(texSampTypes[i], editor.MakeId(), bindVars[i]));
        nbdspv::Id loadedSampler =
            cases.add(nbdspv::OpLoad(texSampTypes[sampIdx], editor.MakeId(), bindVars[sampIdx]));

        nbdspv::Id mergeLabel = editor.MakeId();
        nbdspv::Id gradCase = editor.MakeId();
        nbdspv::Id lodCase = editor.MakeId();
        cases.add(nbdspv::OpSelectionMerge(mergeLabel, nbdspv::SelectionControl::None));
        cases.add(nbdspv::OpBranchConditional(useGradOrGatherOffsets, gradCase, lodCase));

        nbdspv::Id lodResult;
        {
          cases.add(nbdspv::OpLabel(lodCase));
          nbdspv::ImageOperandsAndParamDatas operands = imageOperandsWithOffsets;
          operands.setLod(lod);
          nbdspv::Id combined = cases.add(nbdspv::OpSampledImage(
              texSampCombinedTypes[i], editor.MakeId(), loadedImage, loadedSampler));

          lodResult = cases.add(nbdspv::OpImageSampleExplicitLod(resultType, editor.MakeId(),
                                                                 combined, coord[i], operands));

          cases.add(nbdspv::OpBranch(mergeLabel));
        }

        nbdspv::Id gradResult;
        {
          cases.add(nbdspv::OpLabel(gradCase));
          nbdspv::ImageOperandsAndParamDatas operands = imageOperandsWithOffsets;
          operands.setGrad(ddxs[i], ddys[i]);
          if(m_pDriver->GetDeviceEnabledFeatures().shaderResourceMinLod)
            operands.setMinLod(minlod);
          nbdspv::Id combined = cases.add(nbdspv::OpSampledImage(
              texSampCombinedTypes[i], editor.MakeId(), loadedImage, loadedSampler));

          gradResult = cases.add(nbdspv::OpImageSampleExplicitLod(resultType, editor.MakeId(),
                                                                  combined, coord[i], operands));

          cases.add(nbdspv::OpBranch(mergeLabel));
        }

        cases.add(nbdspv::OpLabel(mergeLabel));
        nbdspv::Id sampleResult = cases.add(nbdspv::OpPhi(
            resultType, editor.MakeId(), {{lodResult, lodCase}, {gradResult, gradCase}}));
        cases.add(nbdspv::OpStore(outVar, sampleResult));
        cases.add(nbdspv::OpBranch(breakLabel));
      }

      // on Qualcomm we only emit Dref instructions against 2D textures, otherwise the compiler may
      // crash.
      if(m_pDriver->GetDriverInfo().QualcommDrefNon2DCompileCrash())
        depthTex &= (i == (uint32_t)ShaderDebugBind::Tex2D);

      // VUID-StandaloneSpirv-OpImage-04777
      // OpImage*Dref must not consume an image whose Dim is 3D
      if(i == (uint32_t)ShaderDebugBind::Tex3D)
        depthTex = false;

      // don't emit dref's for uint/sint textures
      if(uintTex || sintTex)
        depthTex = false;

      if(depthTex)
      {
        for(nbdspv::Op op :
            {nbdspv::Op::ImageSampleDrefExplicitLod, nbdspv::Op::ImageSampleDrefImplicitLod})
        {
          nbdspv::Id label = editor.MakeId();
          targets.push_back({(uint32_t)op * 10 + i, label});

          cases.add(nbdspv::OpLabel(label));
          nbdspv::Id loadedImage =
              cases.add(nbdspv::OpLoad(texSampTypes[i], editor.MakeId(), bindVars[i]));
          nbdspv::Id loadedSampler =
              cases.add(nbdspv::OpLoad(texSampTypes[sampIdx], editor.MakeId(), bindVars[sampIdx]));

          nbdspv::Id mergeLabel = editor.MakeId();
          nbdspv::Id gradCase = editor.MakeId();
          nbdspv::Id lodCase = editor.MakeId();
          cases.add(nbdspv::OpSelectionMerge(mergeLabel, nbdspv::SelectionControl::None));
          cases.add(nbdspv::OpBranchConditional(useGradOrGatherOffsets, gradCase, lodCase));

          nbdspv::Id lodResult;
          {
            cases.add(nbdspv::OpLabel(lodCase));
            nbdspv::ImageOperandsAndParamDatas operands = imageOperandsWithOffsets;
            operands.setLod(lod);
            nbdspv::Id combined = cases.add(nbdspv::OpSampledImage(
                texSampCombinedTypes[i], editor.MakeId(), loadedImage, loadedSampler));

            lodResult = cases.add(nbdspv::OpImageSampleDrefExplicitLod(
                scalarResultType, editor.MakeId(), combined, coord[i], compare, operands));

            cases.add(nbdspv::OpBranch(mergeLabel));
          }

          nbdspv::Id gradResult;
          {
            cases.add(nbdspv::OpLabel(gradCase));
            nbdspv::ImageOperandsAndParamDatas operands = imageOperandsWithOffsets;
            operands.setGrad(ddxs[i], ddys[i]);
            if(m_pDriver->GetDeviceEnabledFeatures().shaderResourceMinLod)
              operands.setMinLod(minlod);
            nbdspv::Id combined = cases.add(nbdspv::OpSampledImage(
                texSampCombinedTypes[i], editor.MakeId(), loadedImage, loadedSampler));

            gradResult = cases.add(nbdspv::OpImageSampleDrefExplicitLod(
                scalarResultType, editor.MakeId(), combined, coord[i], compare, operands));

            cases.add(nbdspv::OpBranch(mergeLabel));
          }

          cases.add(nbdspv::OpLabel(mergeLabel));
          nbdspv::Id scalarSampleResult = cases.add(nbdspv::OpPhi(
              scalarResultType, editor.MakeId(), {{lodResult, lodCase}, {gradResult, gradCase}}));
          nbdspv::Id sampleResult = cases.add(nbdspv::OpCompositeConstruct(
              resultType, editor.MakeId(), {scalarSampleResult, zerof, zerof, zerof}));
          cases.add(nbdspv::OpStore(outVar, sampleResult));
          cases.add(nbdspv::OpBranch(breakLabel));
        }
      }

      // can only gather with 2D/Cube textures
      if(i == (uint32_t)ShaderDebugBind::Tex1D || i == (uint32_t)ShaderDebugBind::Tex3D)
        continue;

      for(nbdspv::Op op : {nbdspv::Op::ImageGather, nbdspv::Op::ImageDrefGather})
      {
        if(op == nbdspv::Op::ImageDrefGather && !depthTex)
          continue;

        nbdspv::Id label = editor.MakeId();
        targets.push_back({(uint32_t)op * 10 + i, label});

        cases.add(nbdspv::OpLabel(label));
        nbdspv::Id loadedImage =
            cases.add(nbdspv::OpLoad(texSampTypes[i], editor.MakeId(), bindVars[i]));
        nbdspv::Id loadedSampler =
            cases.add(nbdspv::OpLoad(texSampTypes[sampIdx], editor.MakeId(), bindVars[sampIdx]));

        nbdspv::Id sampleResult;
        if(m_pDriver->GetDeviceEnabledFeatures().shaderImageGatherExtended)
        {
          nbdspv::Id mergeLabel = editor.MakeId();
          nbdspv::Id constsCase = editor.MakeId();
          nbdspv::Id baseCase = editor.MakeId();
          cases.add(nbdspv::OpSelectionMerge(mergeLabel, nbdspv::SelectionControl::None));
          cases.add(nbdspv::OpBranchConditional(useGradOrGatherOffsets, constsCase, baseCase));

          nbdspv::Id baseResult;
          {
            cases.add(nbdspv::OpLabel(baseCase));

            nbdspv::ImageOperandsAndParamDatas operands;

            if(dynoffset[i] != nbdspv::Id())
              operands.setOffset(dynoffset[i]);

            nbdspv::Id combined = cases.add(nbdspv::OpSampledImage(
                texSampCombinedTypes[i], editor.MakeId(), loadedImage, loadedSampler));

            if(op == nbdspv::Op::ImageGather)
              baseResult = cases.add(nbdspv::OpImageGather(resultType, editor.MakeId(), combined,
                                                           coord[i], gatherChannel, operands));
            else
              baseResult = cases.add(nbdspv::OpImageDrefGather(
                  resultType, editor.MakeId(), combined, coord[i], compare, operands));

            cases.add(nbdspv::OpBranch(mergeLabel));
          }

          nbdspv::Id constsResult;
          {
            cases.add(nbdspv::OpLabel(constsCase));
            nbdspv::ImageOperandsAndParamDatas operands;    // don't use the offsets above

            // if this feature isn't available, this path will never be exercised (since we only
            // come in here when the actual shader used const offsets) so it's fine to drop it in
            // that case to ensure the module is still legal.
            if(m_pDriver->GetDeviceEnabledFeatures().shaderImageGatherExtended &&
               i != (uint32_t)ShaderDebugBind::TexCube)
              operands.setConstOffsets(gatherOffsets);

            nbdspv::Id combined = cases.add(nbdspv::OpSampledImage(
                texSampCombinedTypes[i], editor.MakeId(), loadedImage, loadedSampler));

            if(op == nbdspv::Op::ImageGather)
              constsResult = cases.add(nbdspv::OpImageGather(resultType, editor.MakeId(), combined,
                                                             coord[i], gatherChannel, operands));
            else
              constsResult = cases.add(nbdspv::OpImageDrefGather(
                  resultType, editor.MakeId(), combined, coord[i], compare, operands));

            cases.add(nbdspv::OpBranch(mergeLabel));
          }

          cases.add(nbdspv::OpLabel(mergeLabel));
          sampleResult = cases.add(nbdspv::OpPhi(
              resultType, editor.MakeId(), {{baseResult, baseCase}, {constsResult, constsCase}}));
        }
        else
        {
          nbdspv::ImageOperandsAndParamDatas operands = imageOperandsWithOffsets;

          nbdspv::Id combined = cases.add(nbdspv::OpSampledImage(
              texSampCombinedTypes[i], editor.MakeId(), loadedImage, loadedSampler));

          if(op == nbdspv::Op::ImageGather)
            sampleResult = cases.add(nbdspv::OpImageGather(resultType, editor.MakeId(), combined,
                                                           coord[i], gatherChannel, operands));
          else
            sampleResult = cases.add(nbdspv::OpImageDrefGather(
                resultType, editor.MakeId(), combined, coord[i], compare, operands));
        }

        cases.add(nbdspv::OpStore(outVar, sampleResult));
        cases.add(nbdspv::OpBranch(breakLabel));
      }
    }

    func.add(nbdspv::OpSelectionMerge(breakLabel, nbdspv::SelectionControl::None));
    func.add(nbdspv::OpSwitch32(switchVal, defaultLabel, targets));

    func.append(cases);

    // default: store NULL data
    func.add(nbdspv::OpLabel(defaultLabel));
    func.add(nbdspv::OpStore(
        outVar, editor.AddConstant(nbdspv::OpConstantNull(resultType, editor.MakeId()))));
    func.add(nbdspv::OpBranch(breakLabel));

    func.add(nbdspv::OpLabel(breakLabel));
    func.add(nbdspv::OpReturn());
    func.add(nbdspv::OpFunctionEnd());

    editor.AddFunction(func);
  }

  const uint64_t deviceThreadID;
};

enum class InputSpecConstant
{
  Address = 0,
  AddressMSB,
  ArrayLength,
  DestX,
  DestY,
  DestThreadIDX,
  DestThreadIDY,
  DestThreadIDZ,
  DestInstance,
  DestVertex,
  DestView,
  Count,
};

struct SpecData
{
  VkDeviceAddress bufferAddress;
  uint32_t arrayLength;
  uint32_t destVertex;
  uint32_t destInstance;
  uint32_t destView;
  float destX;
  float destY;
  uint32_t globalThreadIdX;
  uint32_t globalThreadIdY;
  uint32_t globalThreadIdZ;
};

static const VkSpecializationMapEntry specMapsTemplate[] = {
    {
        (uint32_t)InputSpecConstant::Address,
        offsetof(SpecData, bufferAddress),
        // EXT_bda uses a 64-bit constant, as well as KHR_bda64
        0,
    },
    {
        (uint32_t)InputSpecConstant::ArrayLength,
        offsetof(SpecData, arrayLength),
        sizeof(SpecData::arrayLength),
    },
    {
        (uint32_t)InputSpecConstant::DestX,
        offsetof(SpecData, destX),
        sizeof(SpecData::destX),
    },
    {
        (uint32_t)InputSpecConstant::DestY,
        offsetof(SpecData, destY),
        sizeof(SpecData::destY),
    },
    {
        (uint32_t)InputSpecConstant::DestThreadIDX,
        offsetof(SpecData, globalThreadIdX),
        sizeof(SpecData::globalThreadIdX),
    },
    {
        (uint32_t)InputSpecConstant::DestThreadIDY,
        offsetof(SpecData, globalThreadIdY),
        sizeof(SpecData::globalThreadIdY),
    },
    {
        (uint32_t)InputSpecConstant::DestThreadIDZ,
        offsetof(SpecData, globalThreadIdZ),
        sizeof(SpecData::globalThreadIdZ),
    },
    {
        (uint32_t)InputSpecConstant::DestInstance,
        offsetof(SpecData, destInstance),
        sizeof(SpecData::destInstance),
    },
    {
        (uint32_t)InputSpecConstant::DestVertex,
        offsetof(SpecData, destVertex),
        sizeof(SpecData::destVertex),
    },
    {
        (uint32_t)InputSpecConstant::DestView,
        offsetof(SpecData, destView),
        sizeof(SpecData::destView),
    },
    {
        (uint32_t)InputSpecConstant::AddressMSB,
        offsetof(SpecData, bufferAddress) + 4,
        sizeof(uint32_t),
    },
};

NBDCOMPILE_ASSERT((size_t)InputSpecConstant::Count == ARRAY_COUNT(specMapsTemplate),
                  "Spec constants changed");

enum class SubgroupCapability : uint32_t
{
  None = 0,
  EXTBallot,
  Vulkan1_1_NoBallot,
  Vulkan1_1,
};

static const uint32_t validMagicNumber = 12345;
static const uint32_t NumReservedBindings = 1;

// we use the message passing method from the quadoverdraw to swap data between quad neighbours
// using fine derivatives. This is based on "Shader Amortization using Pixel Quad Message Passing",
// Eric Penner, GPU Pro 2.
//
// broadly, if we take ddx_fine and either add or subtract it we can swap horizontal information,
// and similarly vertical for ddy_fine. To swap across the diagonal we perform one fine derivative
// swap, and then use the other fine derivative on the result of that swap.
//
// +---+---+
// | 0 | 1 |
// +---+---+
// | 2 | 3 |
// +---+---+
//
// fine derivatives are obtained by subtracting the right-most neighbour from left-most in each row,
// and the bottom-most neighbour from top-most in each column.
//
// the pseudocode is as follows (following the quad overdraw closely) where X is the type of our value:
//
//
// bool quadX = (quadLaneIndex & 1) != 0;
// bool quadY = (quadLaneIndex & 2) != 0;
//
// bool readX = (readIndex & 1) != 0;
// bool readY = (readIndex & 2) != 0;
//
// float sign_x = quadX ? -1 : 1;
// float sign_y = quadY ? -1 : 1;
//
// X c1 = c0 + sign_x * ddx_fine(c0);
// X c2 = c0 + sign_y * ddy_fine(c0);
// X c3 = c2 + sign_x * ddx_fine(c2);
//
// if(readIndex == quadLaneIndex) // identity, handle gracefully
//   return c0;
// else if(readY == quadY) // horizontal neighbour
//   return c1;
// else if(readX == quadX) // vertical neighbour
//	return c2;
// else
//   return c3; // diagonal neighbour
//

static nbdspv::Id AddQuadSwizzleHelper(nbdspv::Editor &editor, uint32_t count)
{
  nbdspv::Id func = editor.MakeId();

  nbdspv::OperationList ops;

  nbdspv::Id u32 = editor.DeclareType(nbdspv::scalar<uint32_t>());

  nbdspv::Id type;
  if(count == 1)
    type = editor.DeclareType(nbdspv::scalar<float>());
  else
    type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), count));

  nbdspv::Id funcType = editor.DeclareType(nbdspv::FunctionType(type, {type, u32, u32}));

  ops.add(nbdspv::OpFunction(type, func, nbdspv::FunctionControl::None, funcType));
  nbdspv::Id c0 = ops.add(nbdspv::OpFunctionParameter(type, editor.MakeId()));
  nbdspv::Id quadLaneIndex = ops.add(nbdspv::OpFunctionParameter(u32, editor.MakeId()));
  nbdspv::Id readIndex = ops.add(nbdspv::OpFunctionParameter(u32, editor.MakeId()));
  ops.add(nbdspv::OpLabel(editor.MakeId()));

  editor.SetName(c0, "c0");
  editor.SetName(quadLaneIndex, "quadLaneIndex");
  editor.SetName(readIndex, "readIndex");

  nbdspv::Id zero = editor.AddConstantImmediate<uint32_t>(0U);
  nbdspv::Id one = editor.AddConstantImmediate<uint32_t>(1U);
  nbdspv::Id two = editor.AddConstantImmediate<uint32_t>(2U);

  nbdspv::Id posOne = editor.AddConstantImmediate<float>(1.0f);
  nbdspv::Id negOne = editor.AddConstantImmediate<float>(-1.0f);

  nbdspv::Id boolType = editor.DeclareType(nbdspv::scalar<bool>());

  nbdspv::Id quadX = ops.add(nbdspv::OpBitwiseAnd(u32, editor.MakeId(), quadLaneIndex, one));
  quadX = ops.add(nbdspv::OpINotEqual(boolType, editor.MakeId(), quadX, zero));
  nbdspv::Id quadY = ops.add(nbdspv::OpBitwiseAnd(u32, editor.MakeId(), quadLaneIndex, two));
  quadY = ops.add(nbdspv::OpINotEqual(boolType, editor.MakeId(), quadY, zero));

  nbdspv::Id readX = ops.add(nbdspv::OpBitwiseAnd(u32, editor.MakeId(), readIndex, one));
  readX = ops.add(nbdspv::OpINotEqual(boolType, editor.MakeId(), readX, zero));
  nbdspv::Id readY = ops.add(nbdspv::OpBitwiseAnd(u32, editor.MakeId(), readIndex, two));
  readY = ops.add(nbdspv::OpINotEqual(boolType, editor.MakeId(), readY, zero));

  nbdspv::Id horizNeighbour =
      ops.add(nbdspv::OpLogicalEqual(boolType, editor.MakeId(), readY, quadY));
  nbdspv::Id vertNeighbour = ops.add(nbdspv::OpLogicalEqual(boolType, editor.MakeId(), readX, quadX));
  nbdspv::Id isIdentity =
      ops.add(nbdspv::OpIEqual(boolType, editor.MakeId(), quadLaneIndex, readIndex));

  editor.SetName(quadX, "quadX");
  editor.SetName(quadY, "quadY");
  editor.SetName(readX, "readX");
  editor.SetName(readY, "readY");

  if(count >= 2)
  {
    nbdspv::Id floatNtype = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), count));
    nbdspv::Id boolNtype = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<bool>(), count));

    nbdarray<nbdspv::Id> bcast;

    bcast.fill(count, posOne);
    posOne = ops.add(nbdspv::OpCompositeConstruct(floatNtype, editor.MakeId(), bcast));
    bcast.fill(count, negOne);
    negOne = ops.add(nbdspv::OpCompositeConstruct(floatNtype, editor.MakeId(), bcast));

    bcast.fill(count, quadX);
    quadX = ops.add(nbdspv::OpCompositeConstruct(boolNtype, editor.MakeId(), bcast));
    bcast.fill(count, quadY);
    quadY = ops.add(nbdspv::OpCompositeConstruct(boolNtype, editor.MakeId(), bcast));
  }

  nbdspv::Id sign_x = ops.add(nbdspv::OpSelect(type, editor.MakeId(), quadX, negOne, posOne));
  nbdspv::Id sign_y = ops.add(nbdspv::OpSelect(type, editor.MakeId(), quadY, negOne, posOne));
  editor.SetName(sign_x, "sign_x");
  editor.SetName(sign_y, "sign_y");

  nbdspv::Id ddxFine = ops.add(nbdspv::OpDPdxFine(type, editor.MakeId(), c0));
  nbdspv::Id ddyFine = ops.add(nbdspv::OpDPdyFine(type, editor.MakeId(), c0));

  nbdspv::Id c1 = ops.add(nbdspv::OpFMul(type, editor.MakeId(), sign_x, ddxFine));
  c1 = ops.add(nbdspv::OpFAdd(type, editor.MakeId(), c0, c1));
  editor.SetName(c1, "c1");

  nbdspv::Id c2 = ops.add(nbdspv::OpFMul(type, editor.MakeId(), sign_y, ddyFine));
  c2 = ops.add(nbdspv::OpFAdd(type, editor.MakeId(), c0, c2));
  editor.SetName(c2, "c2");

  nbdspv::Id ddxC2 = ops.add(nbdspv::OpDPdxFine(type, editor.MakeId(), c2));

  nbdspv::Id c3 = ops.add(nbdspv::OpFMul(type, editor.MakeId(), sign_x, ddxC2));
  c3 = ops.add(nbdspv::OpFAdd(type, editor.MakeId(), c2, c3));
  editor.SetName(c3, "c3");

  nbdspv::Id trueBranch = editor.MakeId(), falseBranch = editor.MakeId(),
             mergeBranch = editor.MakeId();

  // we'll want to read lane 0 on all lanes for the quadId, so handle that specially
  ops.add(nbdspv::OpSelectionMerge(mergeBranch, nbdspv::SelectionControl::None));
  ops.add(nbdspv::OpBranchConditional(isIdentity, trueBranch, falseBranch));

  ops.add(nbdspv::OpLabel(trueBranch));
  ops.add(nbdspv::OpReturnValue(c0));
  ops.add(nbdspv::OpLabel(falseBranch));
  ops.add(nbdspv::OpBranch(mergeBranch));
  ops.add(nbdspv::OpLabel(mergeBranch));

  // expanded flow control. Impossible labels happen after returns on both branches
  //
  // if(isIdentity) {
  //   ident;
  // }
  // notident;
  //
  // if(horizNeighbour) {
  //   horiz;
  // } else {
  //   notHoriz;
  //   if(vertNeighbour) { vert; } else { diagonal; }
  //   impossible1;
  // }
  // impossible2;
  nbdspv::Id horiz = editor.MakeId(), notHoriz = editor.MakeId(), vert = editor.MakeId(),
             diagonal = editor.MakeId(), imposs1 = editor.MakeId(), imposs2 = editor.MakeId();

  ops.add(nbdspv::OpSelectionMerge(imposs2, nbdspv::SelectionControl::None));
  ops.add(nbdspv::OpBranchConditional(horizNeighbour, horiz, notHoriz));

  ops.add(nbdspv::OpLabel(horiz));
  ops.add(nbdspv::OpReturnValue(c1));
  ops.add(nbdspv::OpLabel(notHoriz));

  ops.add(nbdspv::OpSelectionMerge(imposs1, nbdspv::SelectionControl::None));
  ops.add(nbdspv::OpBranchConditional(vertNeighbour, vert, diagonal));

  ops.add(nbdspv::OpLabel(vert));
  ops.add(nbdspv::OpReturnValue(c2));
  ops.add(nbdspv::OpLabel(diagonal));
  ops.add(nbdspv::OpReturnValue(c3));

  ops.add(nbdspv::OpLabel(imposs1));
  ops.add(nbdspv::OpUnreachable());
  ops.add(nbdspv::OpLabel(imposs2));
  ops.add(nbdspv::OpUnreachable());

  ops.add(nbdspv::OpFunctionEnd());

  editor.AddFunction(ops);
  editor.SetName(func, "quadSwizzleHelper");

  return func;
}

static void CreateInputFetcher(nbdarray<uint32_t> &spv, const nbdarray<SpecConstant> &userSpec,
                               VulkanCreationInfo::ShaderModuleReflection &shadRefl,
                               BufferStorageMode storageMode, bool usePrimitiveID, bool useSampleID,
                               bool useViewIndex, SubgroupCapability subgroupCapability,
                               uint32_t maxSubgroupSize)
{
  nbdspv::Editor editor(spv);

  ShaderStage stage = ShaderStage(shadRefl.stageIndex);
  nbdspv::ThreadScope threadScope = shadRefl.patchData.threadScope;

  uint32_t paramAlign = 16;

  for(const SigParameter &sig : shadRefl.refl->inputSignature)
  {
    if(VarTypeByteSize(sig.varType) * sig.compCount > paramAlign)
      paramAlign = 32;
  }

  // conservatively calculate structure stride with full amount for every input element
  uint32_t structStride = (uint32_t)shadRefl.refl->inputSignature.size() * paramAlign;

  switch(stage)
  {
    case ShaderStage::Vertex: structStride += sizeof(nbdspv::VertexLaneData); break;
    case ShaderStage::Pixel: structStride += sizeof(nbdspv::PixelLaneData); break;
    case ShaderStage::Task:
    case ShaderStage::Mesh:
    case ShaderStage::Compute: structStride += sizeof(nbdspv::ComputeLaneData); break;
    default: break;
  }

  if(threadScope & nbdspv::ThreadScope::Subgroup)
  {
    structStride += sizeof(nbdspv::SubgroupLaneData);
  }

  // simulating full subgroups with ballot ability to read other lanes, we read all lanes data
  const bool fullSubgroups = (subgroupCapability == SubgroupCapability::EXTBallot ||
                              subgroupCapability == SubgroupCapability::Vulkan1_1) &&
                             (threadScope & nbdspv::ThreadScope::Subgroup);
  // faking subgroups without reading a subgroup's worth of data but we still read lane index and elect value
  const bool minimalSubgroups = subgroupCapability != SubgroupCapability::None &&
                                (threadScope & nbdspv::ThreadScope::Subgroup);

  editor.Prepare();
  editor.SetBufferStorageMode(storageMode);

  editor.FlattenSpecConstants(userSpec);

  // remove any OpSource
  {
    // remove any OpName that refers to deleted IDs - functions or results
    nbdspv::Iter it = editor.Begin(nbdspv::Section::DebugStringSource);
    nbdspv::Iter end = editor.End(nbdspv::Section::DebugStringSource);
    while(it < end)
    {
      if(it.opcode() == nbdspv::Op::Source || it.opcode() == nbdspv::Op::SourceContinued)
      {
        editor.Remove(it);
      }
      it++;
    }
  }

  editor.OffsetBindingsToMatchReservation(NumReservedBindings);

  // the original entry ID we're patching
  nbdspv::Id originalEntry = editor.FindEntryID({shadRefl.entryPoint, stage});
  // the new wrapped entry function we'll add
  nbdspv::Id entryID = editor.MakeId();

  // repoint the entry declaration
  editor.ChangeEntry(originalEntry, entryID);

  nbdspv::MemoryAccessAndParamDatas alignedAccess;
  alignedAccess.setAligned(sizeof(uint32_t));

  nbdspv::Id uint32Type = editor.DeclareType(nbdspv::scalar<uint32_t>());
  nbdspv::Id sint32Type = editor.DeclareType(nbdspv::scalar<int32_t>());
  nbdspv::Id floatType = editor.DeclareType(nbdspv::scalar<float>());
  nbdspv::Id boolType = editor.DeclareType(nbdspv::scalar<bool>());
  nbdspv::Id uint2Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<uint32_t>(), 2));
  nbdspv::Id uint3Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<uint32_t>(), 3));
  nbdspv::Id uint4Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<uint32_t>(), 4));
  nbdspv::Id float4Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), 4));
  nbdspv::Id float3Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), 3));
  nbdspv::Id float2Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), 2));
  nbdspv::Id bool4Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<bool>(), 4));

  nbdarray<nbdspv::Id> newGlobals;

  nbdspv::Id LaneDataStruct;

  enum ResultBaseMember
  {
    ResultBase_pos,
    ResultBase_prim,
    ResultBase_sample,
    ResultBase_view,
    ResultBase_valid,
    ResultBase_ddxDerivCheck,
    ResultBase_quadLaneIndex,
    ResultBase_laneIndex,
    ResultBase_subgroupSize,
    ResultBase_globalBallot,
    ResultBase_electBallot,
    ResultBase_helperBallot,
    ResultBase_numSubgroups,
    ResultBase_shadRate,
    ResultBase_firstUser,
  };

  struct laneValue
  {
    nbdstr name;
    // index in the LaneData struct
    size_t structIndex;
    // type ID of the value (float4, uint, etc)
    nbdspv::Id type;
    // direct loaded value per-lane
    nbdspv::Id base;

    // for pixel shaders, the quad's worth of swizzled data to load helper info from, whether or not
    // we have subgroups active
    nbdarray<nbdspv::Id> quadSwizzledData;

    // the loadOps to prepare the base value, done separately so we can push each fixed value into
    // an array while creating the struct to all be processed agnostically, instead of separating adding
    // them to the struct from loading them OR interleaving struct definition while preparing our function
    nbdspv::OperationList loadOps;

    bool flat = false;
  };

  nbdarray<laneValue> laneValues;

  nbdspv::Id subgroupScope = editor.AddConstantImmediate<uint32_t>((uint32_t)nbdspv::Scope::Subgroup);

  nbdspv::Id isHelper, quadLaneIndex, quadId;

  {
    nbdarray<nbdspv::StructMember> structMembers;
    uint32_t offset = 0;

    // declare fixed lane data first

    if(threadScope & nbdspv::ThreadScope::Subgroup)
    {
      laneValue elect;
      elect.name = "__rd_globalElect";
      elect.structIndex = structMembers.size();
      elect.type = uint32Type;
      // don't store elect value for legacy KHR path
      if(subgroupCapability == SubgroupCapability::Vulkan1_1 ||
         subgroupCapability == SubgroupCapability::Vulkan1_1_NoBallot)
      {
        elect.base = elect.loadOps.add(
            nbdspv::OpGroupNonUniformElect(boolType, editor.MakeId(), subgroupScope));
        elect.base = elect.loadOps.add(nbdspv::OpSelect(uint32Type, editor.MakeId(), elect.base,
                                                        editor.AddConstantImmediate<uint32_t>(1),
                                                        editor.AddConstantImmediate<uint32_t>(0)));
        editor.SetName(elect.base, elect.name);
      }
      else
      {
        elect.base = editor.AddConstantImmediate<uint32_t>(0);
        elect.flat = true;
      }
      laneValues.push_back(elect);
      structMembers.push_back(
          {uint32Type, elect.name, offset + (uint32_t)offsetof(nbdspv::SubgroupLaneData, elect)});

      // we implicitly only write data for active lanes so we just set isActive to 1 always
      laneValue isActive;
      isActive.name = "__rd_active";
      isActive.structIndex = structMembers.size();
      isActive.type = uint32Type;
      isActive.base = editor.AddConstantImmediate<uint32_t>(1);
      isActive.flat = true;
      laneValues.push_back(isActive);
      structMembers.push_back({uint32Type, isActive.name,
                               offset + (uint32_t)offsetof(nbdspv::SubgroupLaneData, isActive)});

      structMembers.push_back(
          {uint32Type, "__pad", offset + (uint32_t)offsetof(nbdspv::SubgroupLaneData, padding)});
      structMembers.push_back(
          {uint32Type, "__pad",
           uint32_t(offset + offsetof(nbdspv::SubgroupLaneData, padding) + sizeof(uint32_t))});

      offset += sizeof(nbdspv::SubgroupLaneData);
      NBDCOMPILE_ASSERT((sizeof(nbdspv::SubgroupLaneData) / sizeof(Vec4f)) * sizeof(Vec4f) ==
                            sizeof(nbdspv::SubgroupLaneData),
                        "SubgroupLaneData is misaligned, ensure 16-byte aligned");
    }

    if(stage == ShaderStage::Vertex)
    {
      laneValue inst;
      inst.name = "__rd_inst";
      inst.structIndex = structMembers.size();
      inst.type = uint32Type;
      inst.base = editor.AddBuiltinInputLoad(inst.loadOps, newGlobals, stage,
                                             nbdspv::BuiltIn::InstanceIndex, uint32Type);
      editor.SetName(inst.base, inst.name);
      laneValues.push_back(inst);
      structMembers.push_back(
          {uint32Type, inst.name, offset + (uint32_t)offsetof(nbdspv::VertexLaneData, inst)});

      laneValue vert;
      vert.name = "__rd_vert";
      vert.structIndex = structMembers.size();
      vert.type = uint32Type;
      vert.base = editor.AddBuiltinInputLoad(vert.loadOps, newGlobals, stage,
                                             nbdspv::BuiltIn::VertexIndex, uint32Type);
      editor.SetName(vert.base, vert.name);
      laneValues.push_back(vert);
      structMembers.push_back(
          {uint32Type, vert.name, offset + (uint32_t)offsetof(nbdspv::VertexLaneData, vert)});

      if(useViewIndex)
      {
        laneValue view;
        view.name = "__rd_view";
        view.structIndex = structMembers.size();
        view.type = uint32Type;
        view.base = editor.AddBuiltinInputLoad(view.loadOps, newGlobals, stage,
                                               nbdspv::BuiltIn::ViewIndex, uint32Type);
        editor.SetName(view.base, view.name);
        laneValues.push_back(view);
        structMembers.push_back(
            {uint32Type, view.name, offset + (uint32_t)offsetof(nbdspv::VertexLaneData, view)});

        structMembers.push_back(
            {uint32Type, "__pad", offset + (uint32_t)offsetof(nbdspv::VertexLaneData, padding)});
      }
      else
      {
        structMembers.push_back(
            {uint32Type, "__rd_view", offset + (uint32_t)offsetof(nbdspv::VertexLaneData, view)});
        structMembers.push_back(
            {uint32Type, "__pad", offset + (uint32_t)offsetof(nbdspv::VertexLaneData, padding)});
      }

      offset += sizeof(nbdspv::VertexLaneData);
      NBDCOMPILE_ASSERT((sizeof(nbdspv::VertexLaneData) / sizeof(Vec4f)) * sizeof(Vec4f) ==
                            sizeof(nbdspv::VertexLaneData),
                        "VertexLaneData is misaligned, ensure 16-byte aligned");
    }
    else if(stage == ShaderStage::Pixel)
    {
      laneValue fragCoord;
      fragCoord.name = "__rd_pixelPos";
      fragCoord.structIndex = structMembers.size();
      fragCoord.type = float4Type;
      fragCoord.base = editor.AddBuiltinInputLoad(fragCoord.loadOps, newGlobals, stage,
                                                  nbdspv::BuiltIn::FragCoord, float4Type);
      editor.SetName(fragCoord.base, fragCoord.name);
      laneValues.push_back(fragCoord);
      structMembers.push_back({float4Type, fragCoord.name,
                               offset + (uint32_t)offsetof(nbdspv::PixelLaneData, fragCoord)});

      laneValue helper;
      helper.name = "__rd_isHelper";
      helper.structIndex = structMembers.size();
      helper.type = uint32Type;
      helper.base = editor.AddBuiltinInputLoad(helper.loadOps, newGlobals, stage,
                                               nbdspv::BuiltIn::HelperInvocation, boolType);
      helper.base = helper.loadOps.add(nbdspv::OpSelect(uint32Type, editor.MakeId(), helper.base,
                                                        editor.AddConstantImmediate<uint32_t>(1),
                                                        editor.AddConstantImmediate<uint32_t>(0)));
      editor.SetName(helper.base, helper.name);
      laneValues.push_back(helper);
      structMembers.push_back(
          {uint32Type, helper.name, offset + (uint32_t)offsetof(nbdspv::PixelLaneData, isHelper)});

      laneValue quad;
      quad.name = "__rd_quadId";
      quad.structIndex = structMembers.size();
      quad.type = uint32Type;
      quad.flat = true;
      // this will be handled specially similarly to helper
      quad.base = editor.MakeId();
      editor.SetName(quad.base, quad.name);
      laneValues.push_back(quad);
      structMembers.push_back(
          {uint32Type, quad.name, offset + (uint32_t)offsetof(nbdspv::PixelLaneData, quadId)});

      laneValue quadLane;
      quadLane.name = "__rd_quadLane";
      quadLane.structIndex = structMembers.size();
      quadLane.type = uint32Type;
      quadLane.base = editor.MakeId();
      editor.SetName(quadLane.base, quadLane.name);
      laneValues.push_back(quadLane);
      structMembers.push_back({uint32Type, quadLane.name,
                               offset + (uint32_t)offsetof(nbdspv::PixelLaneData, quadLaneIndex)});

      // quad properties will be handled specially
      isHelper = helper.base;
      quadId = quad.base;
      quadLaneIndex = quadLane.base;

      structMembers.push_back(
          {uint32Type, "__pad", offset + (uint32_t)offsetof(nbdspv::PixelLaneData, padding)});

      offset += sizeof(nbdspv::PixelLaneData);
      NBDCOMPILE_ASSERT((sizeof(nbdspv::PixelLaneData) / sizeof(Vec4f)) * sizeof(Vec4f) ==
                            sizeof(nbdspv::PixelLaneData),
                        "PixelLaneData is misaligned, ensure 16-byte aligned");
    }
    else if(stage == ShaderStage::Compute || stage == ShaderStage::Task || stage == ShaderStage::Mesh)
    {
      laneValue threadid;
      threadid.name = "__rd_threadid";
      threadid.structIndex = structMembers.size();
      threadid.type = uint3Type;
      threadid.base = editor.AddBuiltinInputLoad(threadid.loadOps, newGlobals, stage,
                                                 nbdspv::BuiltIn::LocalInvocationId, uint3Type);
      editor.SetName(threadid.base, threadid.name);
      laneValues.push_back(threadid);
      structMembers.push_back({uint3Type, threadid.name,
                               offset + (uint32_t)offsetof(nbdspv::ComputeLaneData, threadid)});

      laneValue subid;
      subid.name = "__rd_subgroupid";
      subid.structIndex = structMembers.size();
      subid.type = uint32Type;
      subid.base = editor.AddBuiltinInputLoad(subid.loadOps, newGlobals, stage,
                                              nbdspv::BuiltIn::SubgroupId, uint32Type);
      editor.SetName(subid.base, subid.name);
      laneValues.push_back(subid);
      structMembers.push_back({uint32Type, subid.name,
                               offset + (uint32_t)offsetof(nbdspv::ComputeLaneData, subIdxInGroup)});

      offset += sizeof(nbdspv::ComputeLaneData);
      NBDCOMPILE_ASSERT((sizeof(nbdspv::ComputeLaneData) / sizeof(Vec4f)) * sizeof(Vec4f) ==
                            sizeof(nbdspv::ComputeLaneData),
                        "ComputeLaneData is misaligned, ensure 16-byte aligned");
    }

    // now add input signature values

    for(size_t i = 0; i < shadRefl.refl->inputSignature.size(); i++)
    {
      const SPIRVInterfaceAccess &access = shadRefl.patchData.inputs[i];
      const SigParameter &param = shadRefl.refl->inputSignature[i];

      nbdspv::Scalar base = nbdspv::scalar(param.varType);

      uint32_t width = (base.width / 8);

      nbdspv::Id loadType;

      if(param.compCount == 1)
        loadType = editor.DeclareType(base);
      else
        loadType = editor.DeclareType(nbdspv::Vector(base, param.compCount));

      nbdspv::Id valueType;

      // treat bools as uints
      if(base.type == nbdspv::Op::TypeBool)
        width = 4;

      // we immediately upconvert any sub-32-bit types
      if(width < 4)
      {
        width = 4;
        base.width = 32;

        if(param.compCount == 1)
          valueType = editor.DeclareType(base);
        else
          valueType = editor.DeclareType(nbdspv::Vector(base, param.compCount));
      }
      else
      {
        valueType = loadType;
      }

      nbdarray<nbdspv::Id> accessIndices;
      for(uint32_t idx : access.accessChain)
        accessIndices.push_back(editor.AddConstantImmediate<uint32_t>(idx));

      nbdspv::Id inputPtrType =
          editor.DeclareType(nbdspv::Pointer(loadType, nbdspv::StorageClass::Input));

      laneValue value;
      value.name = param.varName;
      value.structIndex = structMembers.size();
      value.type = valueType;

      if(value.name.beginsWith("gl_"))
        value.name = "__rd_" + value.name.substr(3);

      // if we have no access chain it's a global pointer of the type we want, so just load
      // straight out of it
      nbdspv::Id ptr;
      if(accessIndices.empty())
        ptr = access.ID;
      else
        ptr = value.loadOps.add(
            nbdspv::OpAccessChain(inputPtrType, editor.MakeId(), access.ID, accessIndices));

      value.base = value.loadOps.add(nbdspv::OpLoad(loadType, editor.MakeId(), ptr));
      if(valueType == boolType)
      {
        valueType = uint32Type;
        // can't store bools directly, need to convert to uint
        value.base = value.loadOps.add(nbdspv::OpSelect(valueType, editor.MakeId(), value.base,
                                                        editor.AddConstantImmediate<uint32_t>(1),
                                                        editor.AddConstantImmediate<uint32_t>(0)));
      }
      if(valueType != loadType)
      {
        if(VarTypeCompType(param.varType) == CompType::Float)
          value.base = value.loadOps.add(nbdspv::OpFConvert(valueType, editor.MakeId(), value.base));
        else if(VarTypeCompType(param.varType) == CompType::SInt)
          value.base = value.loadOps.add(nbdspv::OpSConvert(valueType, editor.MakeId(), value.base));
        else if(VarTypeCompType(param.varType) == CompType::UInt)
          value.base = value.loadOps.add(nbdspv::OpUConvert(valueType, editor.MakeId(), value.base));
      }
      editor.SetName(value.base, StringFormat::Fmt("__rd_base_%zu_%s", i, param.varName.c_str()));
      // non-float inputs are considered flat
      value.flat = VarTypeCompType(param.varType) != CompType::Float;

      // mark this as non-flat so we still derive it for helper lanes as it will vary
      if(param.systemValue == ShaderBuiltin::IndexInSubgroup ||
         param.systemValue == ShaderBuiltin::PackedFragRate)
        value.flat = false;

      laneValues.push_back(value);

      if(valueType == boolType)
        structMembers.push_back({uint32Type, value.name, offset});
      else
        structMembers.push_back({valueType, value.name, offset});
      offset += param.compCount * width;

      // align offset conservatively, to 16-byte aligned. We do this with explicit uints so we can
      // preview with spirv-cross (and because it doesn't cost anything particularly)
      uint32_t paddingWords = ((paramAlign - (offset % 16)) / 4) % 4;
      for(uint32_t p = 0; p < paddingWords; p++)
      {
        structMembers.push_back({uint32Type, "__pad", offset});
        offset += 4;
      }
    }

    NBDASSERT(offset <= structStride);

    LaneDataStruct = editor.DeclareStructType("__rd_LaneData", structMembers);
  }

  nbdspv::Id arrayLength =
      editor.AddSpecConstantImmediate<uint32_t>(1U, (uint32_t)InputSpecConstant::ArrayLength);

  editor.SetName(arrayLength, "arrayLength");

  nbdspv::Id destX = editor.AddSpecConstantImmediate<float>(0.0f, (uint32_t)InputSpecConstant::DestX);
  nbdspv::Id destY = editor.AddSpecConstantImmediate<float>(0.0f, (uint32_t)InputSpecConstant::DestY);

  editor.SetName(destX, "destX");
  editor.SetName(destY, "destY");

  nbdspv::Id destXY = editor.AddConstant(
      nbdspv::OpSpecConstantComposite(float2Type, editor.MakeId(), {destX, destY}));

  editor.SetName(destXY, "destXY");

  nbdspv::Id destThreadIDX =
      editor.AddSpecConstantImmediate<uint32_t>(0, (uint32_t)InputSpecConstant::DestThreadIDX);
  nbdspv::Id destThreadIDY =
      editor.AddSpecConstantImmediate<uint32_t>(0, (uint32_t)InputSpecConstant::DestThreadIDY);
  nbdspv::Id destThreadIDZ =
      editor.AddSpecConstantImmediate<uint32_t>(0, (uint32_t)InputSpecConstant::DestThreadIDZ);

  editor.SetName(destThreadIDX, "destThreadIDX");
  editor.SetName(destThreadIDY, "destThreadIDY");
  editor.SetName(destThreadIDZ, "destThreadIDZ");

  nbdspv::Id destThreadID = editor.AddConstant(nbdspv::OpSpecConstantComposite(
      uint3Type, editor.MakeId(), {destThreadIDX, destThreadIDY, destThreadIDZ}));

  editor.SetName(destThreadID, "destThreadID");

  nbdspv::Id destInstance =
      editor.AddSpecConstantImmediate<uint32_t>(0, (uint32_t)InputSpecConstant::DestInstance);
  nbdspv::Id destVertex =
      editor.AddSpecConstantImmediate<uint32_t>(0, (uint32_t)InputSpecConstant::DestVertex);
  nbdspv::Id destView =
      editor.AddSpecConstantImmediate<uint32_t>(0, (uint32_t)InputSpecConstant::DestView);

  editor.SetName(destInstance, "destInstance");
  editor.SetName(destVertex, "destVertex");
  editor.SetName(destView, "destView");

  nbdspv::Id ResultDataBaseType;

  uint32_t numLanes = 1;

  if(stage == ShaderStage::Pixel)
    numLanes = 4;

  if(threadScope & nbdspv::ThreadScope::Quad)
    numLanes = 4;

  // if we need full subgroup scope (and we have ballots to read the lanes) declare a subgroup's worth of data
  if(fullSubgroups)
    numLanes = NBDMAX(numLanes, maxSubgroupSize);

  // note we don't need to care about workgroup access - that is only possible on compute and we fill
  // in the rest of the workgroup without reading its inputs, since on compute the only subgroup data
  // we need is size + layout and we assume we can figure out the layout with one subgroup's worth of data

  {
    nbdarray<nbdspv::StructMember> members;

    members.push_back({float4Type, "pos", offsetof(nbdspv::ResultDataBase, pos)});
    members.push_back({uint32Type, "prim", offsetof(nbdspv::ResultDataBase, prim)});
    members.push_back({uint32Type, "sample", offsetof(nbdspv::ResultDataBase, sample)});
    members.push_back({uint32Type, "view", offsetof(nbdspv::ResultDataBase, view)});
    members.push_back({uint32Type, "valid", offsetof(nbdspv::ResultDataBase, valid)});
    members.push_back({floatType, "ddxDerivCheck", offsetof(nbdspv::ResultDataBase, ddxDerivCheck)});
    members.push_back({uint32Type, "quadLaneIndex", offsetof(nbdspv::ResultDataBase, quadLaneIndex)});
    members.push_back({uint32Type, "laneIndex", offsetof(nbdspv::ResultDataBase, laneIndex)});
    members.push_back({uint32Type, "subgroupSize", offsetof(nbdspv::ResultDataBase, subgroupSize)});
    members.push_back({uint4Type, "globalBallot", offsetof(nbdspv::ResultDataBase, globalBallot)});
    members.push_back({uint4Type, "electBallot", offsetof(nbdspv::ResultDataBase, electBallot)});
    members.push_back({uint4Type, "helperBallot", offsetof(nbdspv::ResultDataBase, helperBallot)});
    members.push_back({uint32Type, "numSubgroups", offsetof(nbdspv::ResultDataBase, numSubgroups)});
    members.push_back({uint32Type, "shadRate", offsetof(nbdspv::ResultDataBase, shadRate)});

    // uint2 padding

    const uint32_t dataStart = (uint32_t)AlignUp(sizeof(nbdspv::ResultDataBase), sizeof(Vec4f));

    NBDASSERT((structStride % sizeof(Vec4f)) == 0);

    nbdspv::Id LaneDataArray = editor.AddType(nbdspv::OpTypeArray(
        editor.MakeId(), LaneDataStruct, editor.AddConstantImmediate<uint32_t>(numLanes)));
    editor.AddDecoration(nbdspv::OpDecorate(
        LaneDataArray, nbdspv::DecorationParam<nbdspv::Decoration::ArrayStride>(structStride)));

    members.push_back({LaneDataArray, "LaneData", dataStart});

    ResultDataBaseType = editor.DeclareStructType("ResultData", members);
  }

  nbdspv::Id ResultDataRTArray =
      editor.AddType(nbdspv::OpTypeRuntimeArray(editor.MakeId(), ResultDataBaseType));

  editor.AddDecoration(nbdspv::OpDecorate(
      ResultDataRTArray, nbdspv::DecorationParam<nbdspv::Decoration::ArrayStride>(
                             structStride * numLanes + sizeof(nbdspv::ResultDataBase))));

  nbdspv::Id bufBase =
      editor.DeclareStructType("__rd_HitStorage", {
                                                      {uint32Type, "hit_count", 0},
                                                      {uint32Type, "total_count", sizeof(uint32_t)},
                                                      {uint32Type, "dummy", sizeof(uint32_t) * 2},
                                                      // uint padding

                                                      {ResultDataRTArray, "hits", sizeof(Vec4f)},
                                                  });

  nbdspv::StorageClass bufferClass = editor.PrepareAddedBufferAccess();

  nbdpair<nbdspv::Id, nbdspv::Id> hitBuffer = editor.AddBufferVariable(
      newGlobals, bufBase, "__rd_HitBuffer", 0, (uint32_t)InputSpecConstant::Address, 0);

  NBDCOMPILE_ASSERT(
      uint32_t(InputSpecConstant::Address) + 1 == (uint32_t)InputSpecConstant::AddressMSB,
      "Address spec constant IDs must be contiguous with MSB second");

  nbdspv::Id float4InPtr =
      editor.DeclareType(nbdspv::Pointer(float4Type, nbdspv::StorageClass::Input));
  nbdspv::Id float4BufPtr = editor.DeclareType(nbdspv::Pointer(float4Type, bufferClass));

  nbdspv::Id uint32InPtr =
      editor.DeclareType(nbdspv::Pointer(uint32Type, nbdspv::StorageClass::Input));
  nbdspv::Id uint32BufPtr = editor.DeclareType(nbdspv::Pointer(uint32Type, bufferClass));
  nbdspv::Id uint4BufPtr = editor.DeclareType(nbdspv::Pointer(uint4Type, bufferClass));
  nbdspv::Id floatBufPtr = editor.DeclareType(nbdspv::Pointer(floatType, bufferClass));

  nbdspv::Id glsl450 = editor.ImportExtInst("GLSL.std.450");

  // allow pixel shaders to use fine derivatives
  if(stage == ShaderStage::Pixel)
    editor.AddCapability(nbdspv::Capability::DerivativeControl);

  // declare capabilities we might need
  if(threadScope & nbdspv::ThreadScope::Subgroup)
  {
    if(subgroupCapability == SubgroupCapability::None)
    {
      // nothing, ignore, this could only happen if the shader uses only EXT_shader_subgroup_vote
      // which we treat as degenerate
    }
    else if(subgroupCapability == SubgroupCapability::EXTBallot)
    {
      // this should already be present but let's be sure
      editor.AddCapability(nbdspv::Capability::SubgroupBallotKHR);
    }
    else if(subgroupCapability == SubgroupCapability::Vulkan1_1_NoBallot)
    {
      // this should also already be present
      editor.AddCapability(nbdspv::Capability::GroupNonUniform);
    }
    else if(subgroupCapability == SubgroupCapability::Vulkan1_1)
    {
      editor.AddCapability(nbdspv::Capability::GroupNonUniform);

      // add this, the shader might not have used it but we need it to read other lanes
      editor.AddCapability(nbdspv::Capability::GroupNonUniformBallot);
      editor.AddCapability(nbdspv::Capability::GroupNonUniformVote);
    }
  }

  nbdspv::Id vecNType[5] = {nbdspv::Id(), floatType, float2Type, float3Type, float4Type};
  nbdspv::Id quadSwizzleHelper[5] = {};

  if(stage == ShaderStage::Pixel)
  {
    for(uint32_t i = 1; i <= 4; i++)
    {
      quadSwizzleHelper[i] = AddQuadSwizzleHelper(editor, i);
    }
  }

  {
    nbdspv::OperationList ops;

    nbdspv::Id voidType = editor.DeclareType(nbdspv::scalar<void>());
    nbdspv::Id uintPtr = editor.DeclareType(nbdspv::Pointer(uint32Type, bufferClass));

    nbdspv::Id scope = editor.AddConstantImmediate<uint32_t>((uint32_t)nbdspv::Scope::Device);
    nbdspv::Id semantics =
        editor.AddConstantImmediate<uint32_t>((uint32_t)nbdspv::MemorySemantics::AcquireRelease);

    ops.add(nbdspv::OpFunction(voidType, entryID, nbdspv::FunctionControl::None,
                               editor.DeclareType(nbdspv::FunctionType(voidType, {}))));

    nbdspv::Id structPtr;

    ops.add(nbdspv::OpLabel(editor.MakeId()));
    {
      structPtr = editor.LoadBufferVariable(ops, hitBuffer);

      // we store ddx as a derivative check - it is expected to be 1.0 so store that as fixed for other stages
      nbdspv::Id fragCoord, ddxDerivativeCheck = editor.AddConstantImmediate<float>(1.0f);
      nbdspv::Id laneIndex;

      nbdspv::Id shadRate = editor.AddConstantImmediate<uint32_t>(0);

      // identify the candidate thread in a stage-specific way
      nbdspv::Id candidateThread;

      // prepare stage-specific inputs and condition
      if(stage == ShaderStage::Vertex)
      {
        // we should only be fetching data like this for full subgroups
        NBDASSERT(fullSubgroups);

        nbdspv::Id vert = editor.AddBuiltinInputLoad(ops, newGlobals, stage,
                                                     nbdspv::BuiltIn::VertexIndex, uint32Type);
        nbdspv::Id inst = editor.AddBuiltinInputLoad(ops, newGlobals, stage,
                                                     nbdspv::BuiltIn::InstanceIndex, uint32Type);

        nbdspv::Id equalVert = ops.add(nbdspv::OpIEqual(boolType, editor.MakeId(), vert, destVertex));
        nbdspv::Id equalInstance =
            ops.add(nbdspv::OpIEqual(boolType, editor.MakeId(), inst, destInstance));

        candidateThread =
            ops.add(nbdspv::OpLogicalAnd(boolType, editor.MakeId(), equalVert, equalInstance));

        if(useViewIndex)
        {
          nbdspv::Id view = editor.AddBuiltinInputLoad(ops, newGlobals, stage,
                                                       nbdspv::BuiltIn::ViewIndex, uint32Type);
          nbdspv::Id equalView = ops.add(nbdspv::OpIEqual(boolType, editor.MakeId(), view, destView));
          candidateThread =
              ops.add(nbdspv::OpLogicalAnd(boolType, editor.MakeId(), candidateThread, equalView));
        }
      }
      else if(stage == ShaderStage::Pixel)
      {
        fragCoord = editor.AddBuiltinInputLoad(ops, newGlobals, stage, nbdspv::BuiltIn::FragCoord,
                                               float4Type);

        ddxDerivativeCheck = ops.add(nbdspv::OpDPdx(float4Type, editor.MakeId(), fragCoord));
        editor.SetName(ddxDerivativeCheck, "ddxDerivativeCheck");
        ddxDerivativeCheck =
            ops.add(nbdspv::OpCompositeExtract(floatType, editor.MakeId(), ddxDerivativeCheck, {0}));
        editor.SetName(ddxDerivativeCheck, "ddxDerivativeCheck_x");

        // grab x and y
        nbdspv::Id fragXY = ops.add(
            nbdspv::OpVectorShuffle(float2Type, editor.MakeId(), fragCoord, fragCoord, {0, 1}));

        // masks in the quad are usually 1 apart
        nbdspv::Id xmask = editor.AddConstantImmediate<uint32_t>(1);
        nbdspv::Id ymask = editor.AddConstantImmediate<uint32_t>(1);

        // optionally we may need to shift, if shading rate makes the step larger than 1 to ensure
        // we still get the 0, 1, 2, 3 quad indices we expect
        nbdspv::Id xshift, yshift;

        nbdspv::Id half = editor.AddConstantImmediate<float>(0.5f);
        nbdspv::Id half2D = editor.AddConstant(
            nbdspv::OpConstantComposite(float2Type, editor.MakeId(), {half, half}));

        nbdspv::Id destCentre;

        // when fragment shading rate is active, we need to adjust the expected co-ordinates for a
        // quad, as only half/quarter of the pixels will be shaded so our normal calculations won't
        // work.
        //
        // effectively the xmask and ymask above are taken from the shading rate in each direction,
        // and the dest pixel co-ordinate is adjusted. so that instead of
        //
        // destCentre = destXY + 0.5, 0.5
        //
        // we do:
        //
        // roundedDestXY = (destXY >> shadingRateXY) << shadingRateXY
        // destCentre = roundedDestXY + (shadingRateXY/2.0f)
        //
        // to first truncate the lower bits first to get the 'quad' or 'oct-area' and then add on
        // half the shading rate to get to the centre co-ordinate
        //
        // since pixels at 0,0  0,1  1,0  and 1,1 are all shaded as one and the 'centre' becomes 1,1
        // since that is the co-ordinate for exactly in the middle (normally when shading 1,1 at
        // full rate the centre is 1.5, 1.5 as above).
        //
        // xmask/ymask is also adjusted to get the quadLaneIndex based on proportionally larger quads
        if(editor.HasCapability(nbdspv::Capability::FragmentShadingRateKHR))
        {
          shadRate = editor.AddBuiltinInputLoad(ops, newGlobals, stage,
                                                nbdspv::BuiltIn::ShadingRateKHR, uint32Type);

          NBDCOMPILE_ASSERT(uint32_t(nbdspv::FragmentShadingRate::Vertical2Pixels) == 1,
                            "Vertical mask isn't as expected");
          NBDCOMPILE_ASSERT(uint32_t(nbdspv::FragmentShadingRate::Vertical4Pixels) == 2,
                            "Vertical mask isn't as expected");
          NBDCOMPILE_ASSERT(uint32_t(nbdspv::FragmentShadingRate::Horizontal2Pixels) == 4,
                            "Vertical mask isn't as expected");
          NBDCOMPILE_ASSERT(uint32_t(nbdspv::FragmentShadingRate::Horizontal4Pixels) == 8,
                            "Vertical mask isn't as expected");

          const uint32_t vertMask = uint32_t(nbdspv::FragmentShadingRate::Vertical2Pixels |
                                             nbdspv::FragmentShadingRate::Vertical4Pixels);
          const uint32_t horizMask = uint32_t(nbdspv::FragmentShadingRate::Horizontal2Pixels |
                                              nbdspv::FragmentShadingRate::Horizontal4Pixels);

          nbdspv::Id vertRate =
              ops.add(nbdspv::OpBitwiseAnd(uint32Type, editor.MakeId(), shadRate,
                                           editor.AddConstantImmediate<uint32_t>(vertMask)));
          editor.SetName(vertRate, "vertRate");

          // shift horizRate to be in the right spot
          nbdspv::Id horizRate =
              ops.add(nbdspv::OpBitwiseAnd(uint32Type, editor.MakeId(), shadRate,
                                           editor.AddConstantImmediate<uint32_t>(horizMask)));
          horizRate = ops.add(nbdspv::OpShiftRightLogical(
              uint32Type, editor.MakeId(), horizRate, editor.AddConstantImmediate<uint32_t>(2)));
          editor.SetName(horizRate, "horizRate");

          xshift = horizRate;
          yshift = vertRate;

          nbdspv::Id shadingRateXY = ops.add(
              nbdspv::OpCompositeConstruct(uint2Type, editor.MakeId(), {horizRate, vertRate}));
          editor.SetName(shadingRateXY, "shadingRateXY");

          nbdspv::Id destXYint = ops.add(nbdspv::OpConvertFToU(uint2Type, editor.MakeId(), destXY));
          editor.SetName(destXYint, "destXYint");
          nbdspv::Id destXYshifted = ops.add(
              nbdspv::OpShiftRightLogical(uint2Type, editor.MakeId(), destXYint, shadingRateXY));
          nbdspv::Id roundedDestXY = ops.add(
              nbdspv::OpShiftLeftLogical(uint2Type, editor.MakeId(), destXYshifted, shadingRateXY));
          roundedDestXY = ops.add(nbdspv::OpConvertUToF(float2Type, editor.MakeId(), roundedDestXY));
          editor.SetName(roundedDestXY, "roundedDestXY");

          nbdspv::Id one = editor.AddConstantImmediate<uint32_t>(1U);

          // the masks are 1 shifted from the rate, because rate is 0=full rate, 1=2x2, 2=4x4
          // we also need to max with 1
          xmask = ops.add(nbdspv::OpShiftLeftLogical(uint32Type, editor.MakeId(), horizRate,
                                                     editor.AddConstantImmediate<uint32_t>(1)));
          xmask = ops.add(nbdspv::OpGLSL450(uint32Type, editor.MakeId(), glsl450,
                                            nbdspv::GLSLstd450::UMax, {xmask, one}));
          ymask = ops.add(nbdspv::OpShiftLeftLogical(uint32Type, editor.MakeId(), vertRate,
                                                     editor.AddConstantImmediate<uint32_t>(1)));
          ymask = ops.add(nbdspv::OpGLSL450(uint32Type, editor.MakeId(), glsl450,
                                            nbdspv::GLSLstd450::UMax, {ymask, one}));

          nbdspv::Id pixelWidthX = ops.add(nbdspv::OpConvertUToF(floatType, editor.MakeId(), xmask));
          nbdspv::Id pixelWidthY = ops.add(nbdspv::OpConvertUToF(floatType, editor.MakeId(), ymask));
          nbdspv::Id halfPixelX =
              ops.add(nbdspv::OpFMul(floatType, editor.MakeId(), pixelWidthX, half));
          nbdspv::Id halfPixelY =
              ops.add(nbdspv::OpFMul(floatType, editor.MakeId(), pixelWidthY, half));
          nbdspv::Id halfPixelWidth = ops.add(
              nbdspv::OpCompositeConstruct(float2Type, editor.MakeId(), {halfPixelX, halfPixelY}));

          destCentre =
              ops.add(nbdspv::OpFAdd(float2Type, editor.MakeId(), roundedDestXY, halfPixelWidth));
        }
        else
        {
          // without shading rate the pixel centre is just 0.5, 0.5 from the dest integer co-ordinate
          destCentre = ops.add(nbdspv::OpFAdd(float2Type, editor.MakeId(), destXY, half2D));
        }

        editor.SetName(xmask, "xmask");
        editor.SetName(ymask, "ymask");
        editor.SetName(destCentre, "destCentre");

        // figure out the TL pixel's coords and calculate our index relative to it. Assume even top
        // left (towards 0,0) though the spec does not guarantee this is the actual quad

        // int x01 = x & 1;
        nbdspv::Id xInt =
            ops.add(nbdspv::OpCompositeExtract(floatType, editor.MakeId(), fragXY, {0}));
        xInt = ops.add(nbdspv::OpConvertFToU(uint32Type, editor.MakeId(), xInt));
        nbdspv::Id x01 = ops.add(nbdspv::OpBitwiseAnd(uint32Type, editor.MakeId(), xInt, xmask));

        if(xshift)
          x01 = ops.add(nbdspv::OpShiftRightLogical(uint32Type, editor.MakeId(), x01, xshift));

        // int y01 = y & 1;
        nbdspv::Id yInt =
            ops.add(nbdspv::OpCompositeExtract(floatType, editor.MakeId(), fragXY, {1}));
        yInt = ops.add(nbdspv::OpConvertFToU(uint32Type, editor.MakeId(), yInt));
        nbdspv::Id y01 = ops.add(nbdspv::OpBitwiseAnd(uint32Type, editor.MakeId(), yInt, ymask));

        if(yshift)
          y01 = ops.add(nbdspv::OpShiftRightLogical(uint32Type, editor.MakeId(), y01, yshift));

        // int destIdx = x01 + 2 * y01;
        nbdspv::Id sum = ops.add(nbdspv::OpIMul(uint32Type, editor.MakeId(),
                                                editor.AddConstantImmediate<uint32_t>(2), y01));
        ops.add(nbdspv::OpIAdd(uint32Type, quadLaneIndex, sum, x01));
        laneIndex = quadLaneIndex;
        editor.SetName(quadLaneIndex, "quadLaneIndex");

        // subtract frag coord from the destination co-ord in x-y to get relative
        nbdspv::Id fragXYRelative =
            ops.add(nbdspv::OpFSub(float2Type, editor.MakeId(), fragXY, destCentre));

        // abs()
        nbdspv::Id fragXYAbs = ops.add(nbdspv::OpGLSL450(
            float2Type, editor.MakeId(), glsl450, nbdspv::GLSLstd450::FAbs, {fragXYRelative}));

        nbdspv::Id bool2Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<bool>(), 2));

        // less than 0.5 component-wise
        nbdspv::Id inPixelXY =
            ops.add(nbdspv::OpFOrdLessThan(bool2Type, editor.MakeId(), fragXYAbs, half2D));

        // both less than threshold
        candidateThread = ops.add(nbdspv::OpAll(boolType, editor.MakeId(), inPixelXY));
      }
      else if(stage == ShaderStage::Compute || stage == ShaderStage::Task ||
              stage == ShaderStage::Mesh)
      {
        // we should only be fetching data like this for full subgroups
        NBDASSERT(fullSubgroups);

        nbdspv::Id globalThread = editor.AddBuiltinInputLoad(
            ops, newGlobals, stage, nbdspv::BuiltIn::GlobalInvocationId, uint3Type);

        nbdspv::Id bool3Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<bool>(), 3));
        nbdspv::Id equal3 =
            ops.add(nbdspv::OpIEqual(bool3Type, editor.MakeId(), globalThread, destThreadID));

        candidateThread = ops.add(nbdspv::OpAll(boolType, editor.MakeId(), equal3));
      }

      nbdspv::Id quadIdxConst[4] = {
          editor.AddConstantImmediate<uint32_t>(0),
          editor.AddConstantImmediate<uint32_t>(1),
          editor.AddConstantImmediate<uint32_t>(2),
          editor.AddConstantImmediate<uint32_t>(3),
      };

      // load all data per-thread and calculate quad swizzled neighbour data as needed
      for(laneValue &val : laneValues)
      {
        ops.append(val.loadOps);

        // for pixel shaders we always need to grab quad swizzled data.
        // we skip this for values we classify as flat as well as for the magic isHelper/quadLaneIndex
        // which are handled specially and will be fixed up later when we go to store these
        if(stage == ShaderStage::Pixel && val.base != isHelper && val.base != quadLaneIndex &&
           !val.flat)
        {
          val.quadSwizzledData.resize(4);

          const nbdspv::DataType &dataType = editor.GetDataType(val.type);

          if(dataType.IsU32())
          {
            for(uint32_t q = 0; q < 4; q++)
            {
              nbdspv::Id valQ = val.base;
              valQ = ops.add(nbdspv::OpConvertUToF(floatType, editor.MakeId(), valQ));
              valQ = ops.add(nbdspv::OpFunctionCall(floatType, editor.MakeId(), quadSwizzleHelper[1],
                                                    {valQ, quadLaneIndex, quadIdxConst[q]}));
              // named as convenience because spirv-cross declares a variable here and then does the cast at the usage
              editor.SetName(valQ, StringFormat::Fmt("%s_swiz%u", val.name.c_str(), q));

              valQ = ops.add(nbdspv::OpConvertFToU(uint32Type, editor.MakeId(), valQ));
              editor.SetName(valQ, StringFormat::Fmt("%s_swiz%u_u", val.name.c_str(), q));

              val.quadSwizzledData[q] = valQ;
            }
          }
          else if(dataType.IsS32())
          {
            for(uint32_t q = 0; q < 4; q++)
            {
              nbdspv::Id valQ = val.base;
              valQ = ops.add(nbdspv::OpConvertSToF(floatType, editor.MakeId(), valQ));
              valQ = ops.add(nbdspv::OpFunctionCall(floatType, editor.MakeId(), quadSwizzleHelper[1],
                                                    {valQ, quadLaneIndex, quadIdxConst[q]}));
              // named as convenience because spirv-cross declares a variable here and then does the cast at the usage
              editor.SetName(valQ, StringFormat::Fmt("%s_swiz%u", val.name.c_str(), q));

              valQ = ops.add(nbdspv::OpConvertFToS(sint32Type, editor.MakeId(), valQ));
              editor.SetName(valQ, StringFormat::Fmt("%s_swiz%u_u", val.name.c_str(), q));

              val.quadSwizzledData[q] = valQ;
            }
          }
          else
          {
            // all other inputs that aren't uint32 should be floats, otherwise they should have been marked as flat
            NBDASSERT(dataType.scalar().type == nbdspv::Op::TypeFloat);

            uint32_t width = NBDMAX(1U, dataType.vector().count);

            for(uint32_t q = 0; q < 4; q++)
            {
              val.quadSwizzledData[q] = ops.add(
                  nbdspv::OpFunctionCall(vecNType[width], editor.MakeId(), quadSwizzleHelper[width],
                                         {val.base, quadLaneIndex, quadIdxConst[q]}));
              editor.SetName(val.quadSwizzledData[q],
                             StringFormat::Fmt("%s_swiz%u", val.name.c_str(), q));
            }
          }
        }
      }

      nbdspv::Id subgroupSize, numSubgroups, globalBallot, electBallot, helperBallot;

      // if we are doing even minimal subgroups, read the subgroup-relative lane index and subgroup size
      if(minimalSubgroups || fullSubgroups)
      {
        if(subgroupCapability == SubgroupCapability::EXTBallot)
        {
          globalBallot = ops.add(nbdspv::OpSubgroupBallotKHR(
              uint4Type, editor.MakeId(), editor.AddConstantImmediate<bool>(true)));
          electBallot = editor.AddConstant(nbdspv::OpConstantNull(uint4Type, editor.MakeId()));

          if(stage == ShaderStage::Pixel)
          {
            helperBallot = ops.add(nbdspv::OpINotEqual(boolType, editor.MakeId(), isHelper,
                                                       editor.AddConstantImmediate<uint32_t>(0)));
            helperBallot =
                ops.add(nbdspv::OpSubgroupBallotKHR(uint4Type, editor.MakeId(), helperBallot));
          }
          else
          {
            helperBallot = editor.AddConstant(nbdspv::OpConstantNull(uint4Type, editor.MakeId()));
          }
        }
        else
        {
          globalBallot = ops.add(nbdspv::OpGroupNonUniformBallot(
              uint4Type, editor.MakeId(), subgroupScope, editor.AddConstantImmediate<bool>(true)));
          electBallot =
              ops.add(nbdspv::OpGroupNonUniformElect(boolType, editor.MakeId(), subgroupScope));
          electBallot = ops.add(nbdspv::OpGroupNonUniformBallot(uint4Type, editor.MakeId(),
                                                                subgroupScope, electBallot));

          if(stage == ShaderStage::Pixel)
          {
            helperBallot = ops.add(nbdspv::OpINotEqual(boolType, editor.MakeId(), isHelper,
                                                       editor.AddConstantImmediate<uint32_t>(0)));
            helperBallot = ops.add(nbdspv::OpGroupNonUniformBallot(uint4Type, editor.MakeId(),
                                                                   subgroupScope, helperBallot));
          }
          else
          {
            helperBallot = editor.AddConstant(nbdspv::OpConstantNull(uint4Type, editor.MakeId()));
          }
        }

        laneIndex = editor.AddBuiltinInputLoad(
            ops, newGlobals, stage, nbdspv::BuiltIn::SubgroupLocalInvocationId, uint32Type);
        subgroupSize = editor.AddBuiltinInputLoad(ops, newGlobals, stage,
                                                  nbdspv::BuiltIn::SubgroupSize, uint32Type);
        editor.SetName(laneIndex, "laneIndex");
        editor.SetName(subgroupSize, "subgroupSize");

        // subgroup ID & num subgroups is only available for compute
        if(stage == ShaderStage::Compute || stage == ShaderStage::Task || stage == ShaderStage::Mesh)
        {
          numSubgroups = editor.AddBuiltinInputLoad(ops, newGlobals, stage,
                                                    nbdspv::BuiltIn::NumSubgroups, uint32Type);
          editor.SetName(numSubgroups, "numSubgroups");
        }
        else
        {
          numSubgroups = editor.AddConstantImmediate<uint32_t>(0);
        }
      }
      else
      {
        globalBallot = editor.AddConstant(nbdspv::OpConstantNull(uint4Type, editor.MakeId()));
        electBallot = editor.AddConstant(nbdspv::OpConstantNull(uint4Type, editor.MakeId()));
        helperBallot = editor.AddConstant(nbdspv::OpConstantNull(uint4Type, editor.MakeId()));
        subgroupSize = editor.AddConstantImmediate<uint32_t>(0);
        numSubgroups = editor.AddConstantImmediate<uint32_t>(0);
      }
      editor.SetName(globalBallot, "globalBallot");

      // in a pixel shader we need to take extra steps to ensure we get helper data, and it depends
      // on if we're fetching subgroups or not.
      // if we're not fetching subgroups, we always fetch all 4 helpers and store them since that's
      // all our data. if we ARE fetching subgroups, helper lanes will not write their own data so
      // we do that from the candidate thread (only for the helper lanes, conditionally). We also
      // store into the lane index of each quad with subgroups, as opposed to just 0-3 for a plain
      // quad.
      // if we're not in a pixel shader we don't do any of this
      nbdspv::Id isHelperPerQuad[4] = {};
      nbdspv::Id shouldStoreHelperPerQuad[4] = {};
      nbdspv::Id quadLaneStoreIdx[4] = {};

      if(stage == ShaderStage::Pixel)
      {
        // calculate the quadId that we need for pixels, the top-left thread's lane index
        nbdspv::Id quadIdSwizzle =
            ops.add(nbdspv::OpConvertUToF(floatType, editor.MakeId(), laneIndex));
        quadIdSwizzle =
            ops.add(nbdspv::OpFunctionCall(floatType, editor.MakeId(), quadSwizzleHelper[1],
                                           {quadIdSwizzle, quadLaneIndex, quadIdxConst[0]}));

        quadIdSwizzle = ops.add(nbdspv::OpConvertFToU(uint32Type, editor.MakeId(), quadIdSwizzle));
        // add offset so that quad IDs are always non-zero
        quadId = ops.add(nbdspv::OpIAdd(uint32Type, quadId, quadIdSwizzle,
                                        editor.AddConstantImmediate<uint32_t>(10000)));

        for(uint32_t q = 0; q < 4; q++)
        {
          isHelperPerQuad[q] = ops.add(nbdspv::OpConvertUToF(floatType, editor.MakeId(), isHelper));
          isHelperPerQuad[q] =
              ops.add(nbdspv::OpFunctionCall(floatType, editor.MakeId(), quadSwizzleHelper[1],
                                             {isHelperPerQuad[q], quadLaneIndex, quadIdxConst[q]}));
          isHelperPerQuad[q] =
              ops.add(nbdspv::OpConvertFToU(uint32Type, editor.MakeId(), isHelperPerQuad[q]));
        }

        if(fullSubgroups)
        {
          for(uint32_t q = 0; q < 4; q++)
          {
            shouldStoreHelperPerQuad[q] =
                ops.add(nbdspv::OpINotEqual(boolType, editor.MakeId(), isHelperPerQuad[q],
                                            editor.AddConstantImmediate<uint32_t>(0)));

            quadLaneStoreIdx[q] =
                ops.add(nbdspv::OpConvertUToF(floatType, editor.MakeId(), laneIndex));
            quadLaneStoreIdx[q] = ops.add(
                nbdspv::OpFunctionCall(floatType, editor.MakeId(), quadSwizzleHelper[1],
                                       {quadLaneStoreIdx[q], quadLaneIndex, quadIdxConst[q]}));
            quadLaneStoreIdx[q] =
                ops.add(nbdspv::OpConvertFToU(uint32Type, editor.MakeId(), quadLaneStoreIdx[q]));

            editor.SetName(isHelperPerQuad[q], StringFormat::Fmt("isHelper%u", q));
            editor.SetName(shouldStoreHelperPerQuad[q], StringFormat::Fmt("shouldStore%u", q));
            editor.SetName(quadLaneStoreIdx[q], StringFormat::Fmt("quadLaneStoreIdx%u", q));
          }
        }
        else
        {
          for(uint32_t q = 0; q < 4; q++)
          {
            shouldStoreHelperPerQuad[q] = editor.AddConstantImmediate<bool>(true);
            quadLaneStoreIdx[q] = quadIdxConst[q];
          }
        }
      }

      // get a pointer to buffer.hit_count
      nbdspv::Id hit_count = ops.add(nbdspv::OpAccessChain(
          uintPtr, editor.MakeId(), structPtr, {editor.AddConstantImmediate<uint32_t>(0)}));

      // get a pointer to buffer.total_count
      nbdspv::Id total_count = ops.add(nbdspv::OpAccessChain(
          uintPtr, editor.MakeId(), structPtr, {editor.AddConstantImmediate<uint32_t>(1)}));

      editor.SetName(candidateThread, "candidateThread");

      // if we are fetching full subgroups in a pixel shader, we need to know which threads are quad
      // neighbours of the candidate so we can write their quad lane index properly
      nbdspv::Id candidateThreadInQuad;
      if(fullSubgroups && stage == ShaderStage::Pixel)
      {
        nbdspv::Id zeroF = editor.AddConstantImmediate<float>(0.0f);

        // we do a simple check here - if candidateThread is true, or any ddx/ddy is non-zero, we're
        // in the candidate quad

        nbdspv::Id candidateThreadF = ops.add(nbdspv::OpSelect(
            uint32Type, editor.MakeId(), candidateThread, editor.AddConstantImmediate<uint32_t>(1),
            editor.AddConstantImmediate<uint32_t>(0)));
        candidateThreadF =
            ops.add(nbdspv::OpConvertUToF(floatType, editor.MakeId(), candidateThreadF));

        nbdspv::Id candidateThreadDDXFine =
            ops.add(nbdspv::OpDPdxFine(floatType, editor.MakeId(), candidateThreadF));
        candidateThreadDDXFine = ops.add(
            nbdspv::OpFOrdGreaterThan(boolType, editor.MakeId(), candidateThreadDDXFine, zeroF));

        nbdspv::Id candidateThreadDDYFine =
            ops.add(nbdspv::OpDPdyFine(floatType, editor.MakeId(), candidateThreadF));
        candidateThreadDDYFine = ops.add(
            nbdspv::OpFOrdGreaterThan(boolType, editor.MakeId(), candidateThreadDDYFine, zeroF));

        nbdspv::Id candidateThreadDDXCoarse =
            ops.add(nbdspv::OpDPdxCoarse(floatType, editor.MakeId(), candidateThreadF));
        candidateThreadDDXCoarse = ops.add(
            nbdspv::OpFOrdGreaterThan(boolType, editor.MakeId(), candidateThreadDDXCoarse, zeroF));

        nbdspv::Id candidateThreadDDYCoarse =
            ops.add(nbdspv::OpDPdyCoarse(floatType, editor.MakeId(), candidateThreadF));
        candidateThreadDDYCoarse = ops.add(
            nbdspv::OpFOrdGreaterThan(boolType, editor.MakeId(), candidateThreadDDYCoarse, zeroF));

        candidateThreadInQuad = ops.add(nbdspv::OpLogicalOr(
            boolType, editor.MakeId(), candidateThread, candidateThreadDDXFine));
        candidateThreadInQuad = ops.add(nbdspv::OpLogicalOr(
            boolType, editor.MakeId(), candidateThreadInQuad, candidateThreadDDYFine));
        candidateThreadInQuad = ops.add(nbdspv::OpLogicalOr(
            boolType, editor.MakeId(), candidateThreadInQuad, candidateThreadDDXCoarse));
        candidateThreadInQuad = ops.add(nbdspv::OpLogicalOr(
            boolType, editor.MakeId(), candidateThreadInQuad, candidateThreadDDYCoarse));
        editor.SetName(candidateThreadInQuad, "candidateThreadInQuad");
      }

      nbdarray<nbdspv::Id> killLabels;
      killLabels.push_back(editor.MakeId());
      nbdspv::Id writeLabel = editor.MakeId();

      nbdspv::Id writeCondition = candidateThread;

      // if we're doing proper subgroup readback, keep the whole subgroup, otherwise branch non-uniformly
      if(fullSubgroups)
      {
        if(subgroupCapability == SubgroupCapability::Vulkan1_1)
        {
          writeCondition = ops.add(nbdspv::OpGroupNonUniformAny(boolType, editor.MakeId(),
                                                                subgroupScope, candidateThread));
        }
        else
        {
          // KHR path, emulate a vote with any(ballot() != 0u) so we don't depend on the vote
          // extension - we probably can, but don't have to
          writeCondition =
              ops.add(nbdspv::OpSubgroupBallotKHR(uint4Type, editor.MakeId(), candidateThread));
          writeCondition = ops.add(nbdspv::OpINotEqual(
              bool4Type, editor.MakeId(), writeCondition,
              editor.AddConstant(nbdspv::OpConstantNull(uint4Type, editor.MakeId()))));
          writeCondition = ops.add(nbdspv::OpAny(boolType, editor.MakeId(), writeCondition));
        }
      }

      ops.add(nbdspv::OpSelectionMerge(killLabels.back(), nbdspv::SelectionControl::None));
      ops.add(nbdspv::OpBranchConditional(writeCondition, writeLabel, killLabels.back()));

      ops.add(nbdspv::OpLabel(writeLabel));

      // for pixel shaders with subgroups, ensure we mask off helper lanes from the subgroup so they
      // don't take part in the elect
      if(fullSubgroups && stage == ShaderStage::Pixel)
      {
        killLabels.push_back(editor.MakeId());
        writeLabel = editor.MakeId();
        nbdspv::Id helperCondition = ops.add(nbdspv::OpIEqual(
            boolType, editor.MakeId(), isHelper, editor.AddConstantImmediate<uint32_t>(0)));
        ops.add(nbdspv::OpSelectionMerge(killLabels.back(), nbdspv::SelectionControl::None));
        ops.add(nbdspv::OpBranchConditional(helperCondition, writeLabel, killLabels.back()));
        ops.add(nbdspv::OpLabel(writeLabel));
      }

      nbdspv::Id slotAllocLabel = editor.MakeId(), slotMergeLabel = editor.MakeId();

      // for subgroups the whole subgroup is in here, ensure we only alloc a slot with one lane
      if(fullSubgroups)
      {
        nbdspv::Id nonHelperElected;

        if(subgroupCapability == SubgroupCapability::Vulkan1_1)
          nonHelperElected =
              ops.add(nbdspv::OpGroupNonUniformElect(boolType, editor.MakeId(), subgroupScope));
        else
          nonHelperElected = ops.add(nbdspv::OpSubgroupFirstInvocationKHR(
              boolType, editor.MakeId(), editor.AddConstantImmediate<bool>(true)));
        ops.add(nbdspv::OpSelectionMerge(slotMergeLabel, nbdspv::SelectionControl::None));
        ops.add(nbdspv::OpBranchConditional(nonHelperElected, slotAllocLabel, slotMergeLabel));
      }
      else
      {
        ops.add(nbdspv::OpBranch(slotAllocLabel));
      }

      ops.add(nbdspv::OpLabel(slotAllocLabel));

      // increment total_count
      ops.add(nbdspv::OpAtomicIAdd(uint32Type, editor.MakeId(), total_count, scope, semantics,
                                   editor.AddConstantImmediate<uint32_t>(1)));

      // allocate a slot with atomic add
      nbdspv::Id slot =
          ops.add(nbdspv::OpAtomicIAdd(uint32Type, editor.MakeId(), hit_count, scope, semantics,
                                       editor.AddConstantImmediate<uint32_t>(1)));

      editor.SetName(slot, "slotAlloc");

      ops.add(nbdspv::OpBranch(slotMergeLabel));

      ops.add(nbdspv::OpLabel(slotMergeLabel));

      // now if we're in a subgroup we need to broadcast the slot to the whole group, and also OpPhi
      // the previous slot depending on where we got it from
      if(fullSubgroups)
      {
        slot = ops.add(nbdspv::OpPhi(
            uint32Type, editor.MakeId(),
            {{slot, slotAllocLabel}, {editor.AddConstantImmediate<uint32_t>(0U), writeLabel}}));
        editor.SetName(slot, "slotToBroadcast");

        if(subgroupCapability == SubgroupCapability::Vulkan1_1)
          slot = ops.add(nbdspv::OpGroupNonUniformBroadcastFirst(uint32Type, editor.MakeId(),
                                                                 subgroupScope, slot));
        else
          slot = ops.add(nbdspv::OpSubgroupFirstInvocationKHR(uint32Type, editor.MakeId(), slot));
        editor.SetName(slot, "slot");
      }

      nbdspv::Id inRange = ops.add(nbdspv::OpULessThan(boolType, editor.MakeId(), slot, arrayLength));

      killLabels.push_back(editor.MakeId());
      writeLabel = editor.MakeId();
      ops.add(nbdspv::OpSelectionMerge(killLabels.back(), nbdspv::SelectionControl::None));
      ops.add(nbdspv::OpBranchConditional(inRange, writeLabel, killLabels.back()));
      ops.add(nbdspv::OpLabel(writeLabel));

      nbdspv::Id hitptr = editor.DeclareType(nbdspv::Pointer(ResultDataBaseType, bufferClass));

      // get a pointer to the hit for our slot
      nbdspv::Id hit = ops.add(nbdspv::OpAccessChain(
          hitptr, editor.MakeId(), structPtr, {editor.AddConstantImmediate<uint32_t>(3), slot}));

      // store fixed properties. In the subgroup case this needs to be conditional for only the candidate thread
      nbdspv::Id fixedDataLabel = editor.MakeId(), fixedDataMerge = editor.MakeId();

      if(fullSubgroups)
      {
        ops.add(nbdspv::OpSelectionMerge(fixedDataMerge, nbdspv::SelectionControl::None));
        ops.add(nbdspv::OpBranchConditional(candidateThread, fixedDataLabel, fixedDataMerge));
      }
      else
      {
        ops.add(nbdspv::OpBranch(fixedDataLabel));
      }

      ops.add(nbdspv::OpLabel(fixedDataLabel));

      nbdspv::Id storePtr =
          ops.add(nbdspv::OpAccessChain(float4BufPtr, editor.MakeId(), hit,
                                        {editor.AddConstantImmediate<uint32_t>(ResultBase_pos)}));
      if(fragCoord != nbdspv::Id())
        ops.add(nbdspv::OpStore(storePtr, fragCoord, alignedAccess));

      nbdspv::Id primitiveID;
      if(usePrimitiveID)
      {
        primitiveID = editor.AddBuiltinInputLoad(ops, newGlobals, stage,
                                                 nbdspv::BuiltIn::PrimitiveId, uint32Type);
        editor.AddCapability(nbdspv::Capability::Geometry);
      }
      else
      {
        primitiveID = editor.AddConstantImmediate<uint32_t>(0);
      }

      storePtr =
          ops.add(nbdspv::OpAccessChain(uint32BufPtr, editor.MakeId(), hit,
                                        {editor.AddConstantImmediate<uint32_t>(ResultBase_prim)}));
      ops.add(nbdspv::OpStore(storePtr, primitiveID, alignedAccess));

      nbdspv::Id sampleIndex;
      if(useSampleID)
      {
        sampleIndex = editor.AddBuiltinInputLoad(ops, newGlobals, stage, nbdspv::BuiltIn::SampleId,
                                                 uint32Type);
        editor.AddCapability(nbdspv::Capability::SampleRateShading);
      }
      else
      {
        sampleIndex = editor.AddConstantImmediate<uint32_t>(0);
      }

      storePtr =
          ops.add(nbdspv::OpAccessChain(uint32BufPtr, editor.MakeId(), hit,
                                        {editor.AddConstantImmediate<uint32_t>(ResultBase_sample)}));
      ops.add(nbdspv::OpStore(storePtr, sampleIndex, alignedAccess));

      nbdspv::Id viewIndex;
      if(useViewIndex)
      {
        viewIndex = editor.AddBuiltinInputLoad(ops, newGlobals, stage, nbdspv::BuiltIn::ViewIndex,
                                               uint32Type);
        editor.AddCapability(nbdspv::Capability::MultiView);
        editor.AddExtension("SPV_KHR_multiview");
      }
      else
      {
        viewIndex = editor.AddConstantImmediate<uint32_t>(0);
      }

      storePtr =
          ops.add(nbdspv::OpAccessChain(uint32BufPtr, editor.MakeId(), hit,
                                        {editor.AddConstantImmediate<uint32_t>(ResultBase_view)}));
      ops.add(nbdspv::OpStore(storePtr, viewIndex, alignedAccess));

      storePtr =
          ops.add(nbdspv::OpAccessChain(uint32BufPtr, editor.MakeId(), hit,
                                        {editor.AddConstantImmediate<uint32_t>(ResultBase_valid)}));
      ops.add(nbdspv::OpStore(storePtr, editor.AddConstantImmediate(validMagicNumber), alignedAccess));

      // store derivative health check for pixel shaders
      storePtr = ops.add(
          nbdspv::OpAccessChain(floatBufPtr, editor.MakeId(), hit,
                                {editor.AddConstantImmediate<uint32_t>(ResultBase_ddxDerivCheck)}));
      ops.add(nbdspv::OpStore(storePtr, ddxDerivativeCheck, alignedAccess));

      // store the quadLaneIndex (in case it's different to laneIndex)
      storePtr = ops.add(
          nbdspv::OpAccessChain(uint32BufPtr, editor.MakeId(), hit,
                                {editor.AddConstantImmediate<uint32_t>(ResultBase_quadLaneIndex)}));
      if(quadLaneIndex != nbdspv::Id())
        ops.add(nbdspv::OpStore(storePtr, quadLaneIndex, alignedAccess));

      // store the laneIndex
      storePtr = ops.add(
          nbdspv::OpAccessChain(uint32BufPtr, editor.MakeId(), hit,
                                {editor.AddConstantImmediate<uint32_t>(ResultBase_laneIndex)}));
      ops.add(nbdspv::OpStore(storePtr, laneIndex, alignedAccess));

      // if we have them, store subgroup properties, if they're not present they will be 0
      storePtr = ops.add(
          nbdspv::OpAccessChain(uint32BufPtr, editor.MakeId(), hit,
                                {editor.AddConstantImmediate<uint32_t>(ResultBase_subgroupSize)}));
      ops.add(nbdspv::OpStore(storePtr, subgroupSize, alignedAccess));
      storePtr = ops.add(
          nbdspv::OpAccessChain(uint4BufPtr, editor.MakeId(), hit,
                                {editor.AddConstantImmediate<uint32_t>(ResultBase_globalBallot)}));
      ops.add(nbdspv::OpStore(storePtr, globalBallot, alignedAccess));
      storePtr = ops.add(
          nbdspv::OpAccessChain(uint4BufPtr, editor.MakeId(), hit,
                                {editor.AddConstantImmediate<uint32_t>(ResultBase_electBallot)}));
      ops.add(nbdspv::OpStore(storePtr, electBallot, alignedAccess));
      storePtr = ops.add(
          nbdspv::OpAccessChain(uint4BufPtr, editor.MakeId(), hit,
                                {editor.AddConstantImmediate<uint32_t>(ResultBase_helperBallot)}));
      ops.add(nbdspv::OpStore(storePtr, helperBallot, alignedAccess));
      storePtr = ops.add(
          nbdspv::OpAccessChain(uint32BufPtr, editor.MakeId(), hit,
                                {editor.AddConstantImmediate<uint32_t>(ResultBase_numSubgroups)}));
      ops.add(nbdspv::OpStore(storePtr, numSubgroups, alignedAccess));
      storePtr = ops.add(
          nbdspv::OpAccessChain(uint32BufPtr, editor.MakeId(), hit,
                                {editor.AddConstantImmediate<uint32_t>(ResultBase_shadRate)}));
      ops.add(nbdspv::OpStore(storePtr, shadRate, alignedAccess));

      // merge after doing the fixed data section
      ops.add(nbdspv::OpBranch(fixedDataMerge));
      ops.add(nbdspv::OpLabel(fixedDataMerge));

      nbdspv::Id LaneDataPtrType = editor.DeclareType(nbdspv::Pointer(LaneDataStruct, bufferClass));

      // now we conditionally store each helper lane. Only relevant for pixel shaders but we need
      // all helper lanes for all active lanes to ensure we can get derivatives for any of them
      if(stage == ShaderStage::Pixel)
      {
        for(uint32_t q = 0; q < 4; q++)
        {
          nbdspv::Id doHelperLabel = editor.MakeId(), skipHelperLabel = editor.MakeId();

          ops.add(nbdspv::OpSelectionMerge(skipHelperLabel, nbdspv::SelectionControl::None));
          ops.add(nbdspv::OpBranchConditional(shouldStoreHelperPerQuad[q], doHelperLabel,
                                              skipHelperLabel));
          ops.add(nbdspv::OpLabel(doHelperLabel));

          nbdspv::Id outputPtr = ops.add(nbdspv::OpAccessChain(
              LaneDataPtrType, editor.MakeId(), hit,
              {editor.AddConstantImmediate<uint32_t>(ResultBase_firstUser), quadLaneStoreIdx[q]}));

          for(laneValue &val : laneValues)
          {
            nbdspv::Id valueType = val.type;
            if(valueType == boolType)
              valueType = uint32Type;
            nbdspv::Id ptrType = editor.DeclareType(nbdspv::Pointer(valueType, bufferClass));

            nbdspv::Id valPtr = ops.add(nbdspv::OpAccessChain(
                ptrType, editor.MakeId(), outputPtr,
                {editor.AddConstantImmediate<uint32_t>((uint32_t)val.structIndex)}));

            if(val.base == isHelper)
            {
              ops.add(nbdspv::OpStore(valPtr, isHelperPerQuad[q], alignedAccess));
            }
            else if(val.base == quadLaneIndex)
            {
              ops.add(nbdspv::OpStore(valPtr, quadIdxConst[q], alignedAccess));
            }
            else if(val.flat)
            {
              ops.add(nbdspv::OpStore(valPtr, val.base, alignedAccess));
            }
            else
            {
              NBDASSERT(!val.quadSwizzledData.empty());
              ops.add(nbdspv::OpStore(valPtr, val.quadSwizzledData[q], alignedAccess));
            }
          }

          ops.add(nbdspv::OpBranch(skipHelperLabel));
          ops.add(nbdspv::OpLabel(skipHelperLabel));
        }
      }

      // if we have full subgroups, each subgroup now writes its own data here, if we are in a
      // non-pixel shader without subgroups we store the single thread's data here.
      // the non-subgroup pixel shader case is handled above in the helper lanes (which will all store)
      if(fullSubgroups || stage != ShaderStage::Pixel)
      {
        nbdspv::Id idx;

        if(fullSubgroups)
          idx = laneIndex;
        else
          idx = editor.AddConstantImmediate<uint32_t>(0U);

        nbdspv::Id outputPtr = ops.add(nbdspv::OpAccessChain(
            LaneDataPtrType, editor.MakeId(), hit,
            {editor.AddConstantImmediate<uint32_t>(ResultBase_firstUser), idx}));

        for(laneValue &val : laneValues)
        {
          nbdspv::Id valueType = val.type;
          if(valueType == boolType)
            valueType = uint32Type;
          nbdspv::Id ptrType = editor.DeclareType(nbdspv::Pointer(valueType, bufferClass));

          nbdspv::Id valPtr = ops.add(nbdspv::OpAccessChain(
              ptrType, editor.MakeId(), outputPtr,
              {editor.AddConstantImmediate<uint32_t>((uint32_t)val.structIndex)}));

          ops.add(nbdspv::OpStore(valPtr, val.base, alignedAccess));
        }
      }

      // join up with the early-outs we did, in reverse order
      for(size_t i = 0; i < killLabels.size(); i++)
      {
        nbdspv::Id label = killLabels[killLabels.size() - 1 - i];
        ops.add(nbdspv::OpBranch(label));
        ops.add(nbdspv::OpLabel(label));
      }
    }

    // we want to "call" the original function to ensure the compiler does hopefully as close
    // codegen as possible to the original but we don't want to actually execute it. To do this we
    // use an atomic max with a dummy value and only call the function if the value is *larger* -
    // the compiler can't know what value was pre-existing in the buffer (though we know it was
    // zero) so it can't eliminate either branch, but in practice we will always return

    nbdspv::Id trueLabel = editor.MakeId();
    nbdspv::Id falseLabel = editor.MakeId();

    // get a pointer to buffer.dummy
    nbdspv::Id dummy = ops.add(nbdspv::OpAccessChain(uintPtr, editor.MakeId(), structPtr,
                                                     {editor.AddConstantImmediate<uint32_t>(2)}));

    dummy = ops.add(nbdspv::OpAtomicUMax(uint32Type, editor.MakeId(), dummy, scope, semantics,
                                         editor.AddConstantImmediate<uint32_t>(1)));
    editor.SetName(dummy, "dummy");
    nbdspv::Id dummyCompare = ops.add(nbdspv::OpULessThan(
        boolType, editor.MakeId(), dummy, editor.AddConstantImmediate<uint32_t>(2)));

    ops.add(nbdspv::OpSelectionMerge(falseLabel, nbdspv::SelectionControl::None));
    ops.add(nbdspv::OpBranchConditional(dummyCompare, trueLabel, falseLabel));

    ops.add(nbdspv::OpLabel(trueLabel));

    //  don't return, kill. This makes it well-defined that we don't write anything to our outputs
    if(ShaderStage(shadRefl.stageIndex) == ShaderStage::Pixel)
      ops.add(nbdspv::OpKill());
    else
      ops.add(nbdspv::OpReturn());

    ops.add(nbdspv::OpLabel(falseLabel));

    ops.add(nbdspv::OpFunctionCall(voidType, editor.MakeId(), originalEntry));

    ops.add(nbdspv::OpReturn());

    ops.add(nbdspv::OpFunctionEnd());

    editor.AddFunction(ops);
  }

  editor.AddEntryGlobals(entryID, newGlobals);
}

nbdpair<uint32_t, uint32_t> GetAlignAndOutputSize(VulkanCreationInfo::ShaderModuleReflection &shadRefl)
{
  uint32_t paramAlign = 16;

  for(const SigParameter &sig : shadRefl.refl->inputSignature)
  {
    if(VarTypeByteSize(sig.varType) * sig.compCount > paramAlign)
      paramAlign = 32;
  }

  // conservatively calculate structure stride with full amount for every input element
  uint32_t structStride = (uint32_t)shadRefl.refl->inputSignature.size() * paramAlign;

  if(shadRefl.refl->stage == ShaderStage::Vertex)
    structStride += sizeof(nbdspv::VertexLaneData);
  else if(shadRefl.refl->stage == ShaderStage::Pixel)
    structStride += sizeof(nbdspv::PixelLaneData);
  else if(shadRefl.refl->stage == ShaderStage::Compute ||
          shadRefl.refl->stage == ShaderStage::Task || shadRefl.refl->stage == ShaderStage::Mesh)
    structStride += sizeof(nbdspv::ComputeLaneData);

  if(shadRefl.patchData.threadScope & nbdspv::ThreadScope::Subgroup)
  {
    structStride += sizeof(nbdspv::SubgroupLaneData);
  }

  return {paramAlign, structStride};
}

VkDescriptorSetLayoutBinding MakeNewBinding(VkShaderStageFlagBits stage)
{
  return {
      0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL,
  };
}

void VulkanReplay::CalculateSubgroupProperties(uint32_t &maxSubgroupSize,
                                               SubgroupCapability &subgroupCapability)
{
  maxSubgroupSize = 4;

  // if we don't have subgroup ballots we assume we have no real meaningful subgroup capabilities at
  // all except for 'basic'. The only thing basic lets you do is fetch the subgroup ID, and
  // determine which lane is the first active (OpGroupNonUniformElect).
  // in this case we effectively consider it non-subgroup and just read those values directly to
  // fill in, but otherwise simulate as if there were no subgroup use.

  // for our purposes vulkan 1.1 fully deprecated the old EXT_shader_subgroup_* pair of extensions
  // as the only thing that wasn't deprecated was a non-constant broadcast ID which we don't need
  if(m_pDriver->GetExtensions(NULL).vulkanVersion >= VK_API_VERSION_1_1)
  {
    VkPhysicalDeviceSubgroupProperties subProps = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
    };

    VkPhysicalDeviceProperties2 availBase = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    availBase.pNext = &subProps;
    m_pDriver->vkGetPhysicalDeviceProperties2(m_pDriver->GetPhysDev(), &availBase);

    maxSubgroupSize = subProps.subgroupSize;
    subgroupCapability = SubgroupCapability::Vulkan1_1_NoBallot;
    const VkSubgroupFeatureFlags requiredFlags =
        (VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT);
    if((subProps.supportedOperations & requiredFlags) == requiredFlags)
      subgroupCapability = SubgroupCapability::Vulkan1_1;

    if(m_pDriver->GetExtensions(NULL).ext_EXT_subgroup_size_control)
    {
      VkPhysicalDeviceSubgroupSizeControlProperties subSizeProps = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES,
      };
      availBase.pNext = &subSizeProps;
      m_pDriver->vkGetPhysicalDeviceProperties2(m_pDriver->GetPhysDev(), &availBase);

      // use new upper bound in case it's higher with variable sizes
      maxSubgroupSize = NBDMAX(maxSubgroupSize, subSizeProps.maxSubgroupSize);
    }
  }
  else if(m_pDriver->GetExtensions(NULL).ext_EXT_shader_subgroup_ballot)
  {
    // the ballot extension only proides the subgroup size on the GPU so we need to allocate worst case up front

    NBDWARN("Subgroup ballot extension is best extension enabled - using worst case subgroup size");

    maxSubgroupSize = 128;
    subgroupCapability = SubgroupCapability::EXTBallot;
  }
  else if(m_pDriver->GetExtensions(NULL).ext_EXT_shader_subgroup_vote)
  {
    // if only the vote extension is enabled we have no way to determine the subgroup size or
    // anything, so we just fall back to treating this as a degenerate case with a single thread

    NBDWARN("Subgroup vote extension is only subgroup feature enabled - treating as degenerate");

    maxSubgroupSize = 1;
    subgroupCapability = SubgroupCapability::None;
  }
}

VkSpecializationInfo VulkanReplay::MakeSpecInfo(SpecData &specData, VkSpecializationMapEntry *specMaps)
{
  memcpy(specMaps, specMapsTemplate, sizeof(specMapsTemplate));

  specMaps[(uint32_t)InputSpecConstant::Address].size =
      (m_StorageMode == BufferStorageMode::KHR_bda32 ? sizeof(uint32_t) : sizeof(uint64_t));

  VkSpecializationInfo ret = {};
  ret.dataSize = sizeof(specData);
  ret.pData = &specData;
  ret.mapEntryCount = (uint32_t)InputSpecConstant::Count;
  ret.pMapEntries = specMaps;
  return ret;
}

ShaderDebugTrace *VulkanReplay::DebugVertex(uint32_t eventId, uint32_t vertid, uint32_t instid,
                                            uint32_t idx, uint32_t view)
{
  const VulkanRenderState &state = m_pDriver->GetRenderState();
  VulkanCreationInfo &c = m_pDriver->m_CreationInfo;

  nbdstr regionName =
      StringFormat::Fmt("DebugVertex @ %u of (%u,%u,%u,%u)", eventId, vertid, instid, idx, view);

  VkMarkerRegion region(regionName);

  if(Vulkan_Debug_ShaderDebugLogging())
    NBDLOG("%s", regionName.c_str());

  const ActionDescription *action = m_pDriver->GetAction(eventId);

  if(!(action->flags & ActionFlags::Drawcall))
  {
    NBDLOG("No drawcall selected");
    return new ShaderDebugTrace();
  }

  uint32_t vertOffset = 0, instOffset = 0;
  if(!(action->flags & ActionFlags::Indexed))
    vertOffset = action->vertexOffset;

  if(action->flags & ActionFlags::Instanced)
    instOffset = action->instanceOffset;

  // get ourselves in pristine state before this action (without any side effects it may have had)
  m_pDriver->ReplayLog(0, eventId, eReplay_WithoutDraw);

  const VulkanCreationInfo::Pipeline &pipe = c.m_Pipeline[state.graphics.pipeline];
  const VulkanCreationInfo::ShaderEntry &shaderEntry =
      state.graphics.shaderObject ? c.m_ShaderObject[state.shaderObjects[0]].shad : pipe.shaders[0];
  VulkanCreationInfo::ShaderModule &shader = c.m_ShaderModule[shaderEntry.module];
  nbdstr entryPoint = shaderEntry.entryPoint;
  const nbdarray<SpecConstant> &spec = shaderEntry.specialization;

  VulkanCreationInfo::ShaderModuleReflection &shadRefl =
      shader.GetReflection(ShaderStage::Vertex, entryPoint, state.graphics.pipeline);

  if(!shadRefl.refl->debugInfo.debuggable)
  {
    NBDLOG("Shader is not debuggable: %s", shadRefl.refl->debugInfo.debugStatus.c_str());
    return new ShaderDebugTrace();
  }

  if((shadRefl.patchData.threadScope & nbdspv::ThreadScope::Subgroup) &&
     !m_pDriver->GetDeviceEnabledFeatures().vertexPipelineStoresAndAtomics)
  {
    NBDWARN("Subgroup vertex debugging is not supported without vertex stores");
    return new ShaderDebugTrace;
  }

  shadRefl.PopulateDisassembly(shader.spirv);

  VulkanAPIWrapper *apiWrapper =
      new VulkanAPIWrapper(m_pDriver, c, ShaderStage::Vertex, eventId, shadRefl.refl->resourceId);

  // clamp the view index to the number of multiviews, just to be sure
  uint32_t numViews = 1;

  if(state.dynamicRendering.active)
    numViews = state.dynamicRendering.viewMask == ~0U
                   ? 32
                   : NBDMAX(numViews, Log2Ceil(state.dynamicRendering.viewMask + 1));
  else
    numViews =
        (uint32_t)c.m_RenderPass[state.GetRenderPass()].subpasses[state.subpass].multiviews.size();
  if(numViews > 1)
    view = NBDMIN(numViews - 1, view);
  else
    view = 0;

  SubgroupCapability subgroupCapability = SubgroupCapability::None;
  uint32_t maxSubgroupSize = 1;
  CalculateSubgroupProperties(maxSubgroupSize, subgroupCapability);

  uint32_t numThreads = 1;

  if(shadRefl.patchData.threadScope & nbdspv::ThreadScope::Subgroup)
    numThreads = NBDMAX(numThreads, maxSubgroupSize);

  nbdarray<nbdarray<ShaderVariable>> &location_inputs = apiWrapper->GetLocationInputs();
  nbdarray<std::unordered_map<ShaderBuiltin, ShaderVariable>> &allthread_builtins =
      apiWrapper->GetThreadBuiltins();
  location_inputs.resize(numThreads);
  allthread_builtins.resize(numThreads);
  apiWrapper->thread_props.resize(numThreads);

  apiWrapper->thread_props[0][(size_t)nbdspv::ThreadProperty::Active] = 1;

  std::unordered_map<ShaderBuiltin, ShaderVariable> &global_builtins =
      apiWrapper->GetGlobalBuiltins();
  global_builtins[ShaderBuiltin::BaseInstance] =
      ShaderVariable(nbdstr(), action->instanceOffset, 0U, 0U, 0U);
  global_builtins[ShaderBuiltin::BaseVertex] = ShaderVariable(
      nbdstr(), (action->flags & ActionFlags::Indexed) ? action->baseVertex : action->vertexOffset,
      0U, 0U, 0U);
  global_builtins[ShaderBuiltin::DeviceIndex] = ShaderVariable(nbdstr(), 0U, 0U, 0U, 0U);
  global_builtins[ShaderBuiltin::DrawIndex] = ShaderVariable(nbdstr(), action->drawIndex, 0U, 0U, 0U);
  global_builtins[ShaderBuiltin::ViewportIndex] = ShaderVariable(nbdstr(), view, 0U, 0U, 0U);
  global_builtins[ShaderBuiltin::MultiViewIndex] = ShaderVariable(nbdstr(), view, 0U, 0U, 0U);

  bool useViewIndex = (view == ~0U) ? false : true;
  if(useViewIndex)
  {
    ResourceId rp = state.GetRenderPass();
    if(rp != ResourceId())
    {
      const VulkanCreationInfo::RenderPass &rpInfo =
          m_pDriver->GetDebugManager()->GetRenderPassInfo(rp);
      for(auto it = rpInfo.subpasses.begin(); it != rpInfo.subpasses.end(); ++it)
      {
        if(it->multiviews.isEmpty())
        {
          if(Vulkan_Debug_ShaderDebugLogging())
            NBDLOG(
                "Disabling useViewIndex because at least one subpass does not have multiple views");
          useViewIndex = false;
          break;
        }
      }
    }
    else
    {
      useViewIndex =
          (state.dynamicRendering.active ? state.dynamicRendering.viewMask : pipe.viewMask) != 0;
      if(!useViewIndex && Vulkan_Debug_ShaderDebugLogging())
        NBDLOG("Disabling useViewIndex because viewMask is zero");
    }
  }
  else
  {
    if(Vulkan_Debug_ShaderDebugLogging())
      NBDLOG("Disabling useViewIndex from input view %u", view);
  }

  // if we need to fetch subgroup data, do that now.
  uint32_t laneIndex = 0;
  if(numThreads > 1)
  {
    SpecData specData = {};

    if(action->flags & ActionFlags::Indexed)
      specData.destVertex = idx;
    else
      specData.destVertex = vertid + vertOffset;
    specData.destInstance = instid + instOffset;
    specData.destView = view == ~0U ? 0 : view;

    uint32_t paramAlign, structStride;
    nbdtie(paramAlign, structStride) = GetAlignAndOutputSize(shadRefl);

    uint32_t maxHits = 4;    // we should only ever get one hit

    // struct size is nbdspv::ResultDataBase header plus Nx structStride for the number of threads
    uint32_t structSize = sizeof(nbdspv::ResultDataBase) + structStride * numThreads;

    VkDeviceSize feedbackStorageSize = maxHits * structSize + 1024;

    if(Vulkan_Debug_ShaderDebugLogging())
    {
      NBDLOG("Output structure is %u sized, output buffer is %llu bytes", structStride,
             feedbackStorageSize);
    }

    m_PatchedShaderFeedback.ResizeFeedbackBuffer(m_pDriver, feedbackStorageSize);

    specData.arrayLength = maxHits;

    // make copy of state to draw from
    VulkanRenderState modifiedstate = state;

    NBDCOMPILE_ASSERT(NumReservedBindings == 1, "NumReservedBindings is wrong");
    AddedDescriptorData patchedBufferdata = PrepareExtraBufferDescriptor(
        modifiedstate, false, {MakeNewBinding(VK_SHADER_STAGE_VERTEX_BIT)}, false);

    if(patchedBufferdata.empty())
    {
      delete apiWrapper;

      ShaderDebugTrace *ret = new ShaderDebugTrace;
      ret->stage = ShaderStage::Vertex;

      return ret;
    }

    if(!patchedBufferdata.descSets.empty())
      m_PatchedShaderFeedback.FeedbackBuffer.WriteDescriptor(Unwrap(patchedBufferdata.descSets[0]),
                                                             0, 0);

    specData.bufferAddress = m_PatchedShaderFeedback.FeedbackBuffer.Address();
    if(specData.bufferAddress && Vulkan_Debug_ShaderDebugLogging())
    {
      NBDLOG("Got buffer address of %llu", specData.bufferAddress);
    }

    // create shader with modified code

    VkSpecializationMapEntry specMaps[(size_t)InputSpecConstant::Count];
    NBDCOMPILE_ASSERT(sizeof(specMaps) == sizeof(specMapsTemplate),
                      "Specialisation maps have changed");

    VkSpecializationInfo patchedSpecInfo = MakeSpecInfo(specData, specMaps);

    auto patchCallback = [this, &spec, &shadRefl, &patchedSpecInfo, useViewIndex, subgroupCapability,
                          maxSubgroupSize](const AddedDescriptorData &patchedBufferdata,
                                           VkShaderStageFlagBits stage, const char *entryName,
                                           const nbdarray<uint32_t> &origSpirv,
                                           nbdarray<uint32_t> &modSpirv,
                                           const VkSpecializationInfo *&specInfo) {
      if(stage != VK_SHADER_STAGE_VERTEX_BIT)
        return false;

      modSpirv = origSpirv;

      if(!Vulkan_Debug_PSDebugDumpDirPath().empty())
        FileIO::WriteAll(Vulkan_Debug_PSDebugDumpDirPath() + "/debug_vsinput_before.spv", modSpirv);

      CreateInputFetcher(modSpirv, spec, shadRefl, m_StorageMode, false, false, useViewIndex,
                         subgroupCapability, maxSubgroupSize);

      if(!Vulkan_Debug_PSDebugDumpDirPath().empty())
        FileIO::WriteAll(Vulkan_Debug_PSDebugDumpDirPath() + "/debug_vsinput_after.spv", modSpirv);

      // overwrite user's specialisation info. We flattened the user's spec constants when patching the SPIR-V above.
      specInfo = &patchedSpecInfo;

      return true;
    };

    PrepareStateForPatchedShader(patchedBufferdata, modifiedstate, false, patchCallback);

    if(!RunFeedbackAction(feedbackStorageSize, action, modifiedstate))
    {
      delete apiWrapper;

      ShaderDebugTrace *ret = new ShaderDebugTrace;
      ret->stage = ShaderStage::Vertex;

      return ret;
    }

    bytebuf data;
    GetDebugManager()->GetBufferData(m_PatchedShaderFeedback.FeedbackBuffer, 0, 0, data);

    byte *base = data.data();
    uint32_t hit_count = ((uint32_t *)base)[0];
    // uint32_t total_count = ((uint32_t *)base)[1];

    NBDASSERTMSG("Should only get one hit for vertex shaders", hit_count == 1, hit_count);

    base += sizeof(Vec4f);

    nbdspv::ResultDataBase *winner = (nbdspv::ResultDataBase *)base;

    if(winner->valid != validMagicNumber)
    {
      NBDWARN("Hit doesn't have valid magic number");

      delete apiWrapper;

      ShaderDebugTrace *ret = new ShaderDebugTrace;
      ret->stage = ShaderStage::Vertex;

      return ret;
    }

    nbdspv::Debugger *debugger = new nbdspv::Debugger;
    debugger->Parse(shader.spirv.GetSPIRV());

    // the per-thread data immediately follows the nbdspv::ResultDataBase header. Every piece of
    // data is uniformly aligned, either 16-byte by default or 32-byte if larger components exist.
    // The output is in input signature order.
    byte *LaneData = (byte *)(winner + 1);

    numThreads = 4;

    if(shadRefl.patchData.threadScope & nbdspv::ThreadScope::Subgroup)
    {
      NBDASSERTNOTEQUAL(winner->subgroupSize, 0);
      numThreads = NBDMAX(numThreads, winner->subgroupSize);
    }

    location_inputs.resize(numThreads);
    allthread_builtins.resize(numThreads);
    apiWrapper->thread_props.resize(numThreads);

    for(uint32_t t = 0; t < numThreads; t++)
    {
      byte *value = LaneData + t * structStride;

      {
        nbdspv::SubgroupLaneData *subgroupData = (nbdspv::SubgroupLaneData *)value;
        apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::Active] = subgroupData->isActive;
        apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::Elected] = subgroupData->elect;
        apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::SubgroupId] = t;

        value += sizeof(nbdspv::SubgroupLaneData);
      }

      // read VertexLaneData
      {
        nbdspv::VertexLaneData *vertData = (nbdspv::VertexLaneData *)value;

        allthread_builtins[t][ShaderBuiltin::InstanceIndex] =
            ShaderVariable("InstanceIndex"_lit, vertData->inst, 0U, 0U, 0U);
        allthread_builtins[t][ShaderBuiltin::VertexIndex] =
            ShaderVariable("VertexIndex"_lit, vertData->vert, 0U, 0U, 0U);
        allthread_builtins[t][ShaderBuiltin::MultiViewIndex] =
            ShaderVariable("VertexIndex"_lit, vertData->view, 0U, 0U, 0U);

        if(view != ~0U)
          NBDASSERTEQUAL(vertData->view, view);
      }
      value += sizeof(nbdspv::VertexLaneData);

      for(size_t i = 0; i < shadRefl.refl->inputSignature.size(); i++)
      {
        const SigParameter &param = shadRefl.refl->inputSignature[i];

        bool builtin = true;
        if(param.systemValue == ShaderBuiltin::Undefined)
        {
          builtin = false;
          location_inputs[t].resize(NBDMAX((uint32_t)location_inputs.size(), param.regIndex + 1));
        }

        ShaderVariable &var =
            builtin ? allthread_builtins[t][param.systemValue] : location_inputs[t][param.regIndex];

        var.rows = 1;
        var.columns = param.compCount & 0xff;
        var.type = param.varType;

        const uint32_t comp = Bits::CountTrailingZeroes(uint32_t(param.regChannelMask));
        const uint32_t elemSize = VarTypeByteSize(param.varType);

        const size_t sz = elemSize * param.compCount;

        memcpy((var.value.u8v.data()) + elemSize * comp, value + i * paramAlign, sz);
      }
    }

    global_builtins[ShaderBuiltin::SubgroupSize] = ShaderVariable(nbdstr(), numThreads, 0U, 0U, 0U);
    apiWrapper->SetInputVarsToReadOnly();
    ShaderDebugTrace *ret = debugger->BeginDebug(apiWrapper, ShaderStage::Vertex, entryPoint, spec,
                                                 shadRefl.instructionLines, shadRefl.patchData,
                                                 winner->laneIndex, numThreads, numThreads);
    apiWrapper->ResetReplay();

    return ret;
  }
  else
  {
    // otherwise we can do a simple manual fetch of vertex inputs
    laneIndex = 0;

    std::unordered_map<ShaderBuiltin, ShaderVariable> &thread_builtins =
        allthread_builtins[laneIndex];
    if(action->flags & ActionFlags::Indexed)
      thread_builtins[ShaderBuiltin::VertexIndex] = ShaderVariable(nbdstr(), idx, 0U, 0U, 0U);
    else
      thread_builtins[ShaderBuiltin::VertexIndex] =
          ShaderVariable(nbdstr(), vertid + vertOffset, 0U, 0U, 0U);
    thread_builtins[ShaderBuiltin::InstanceIndex] =
        ShaderVariable(nbdstr(), instid + instOffset, 0U, 0U, 0U);

    nbdarray<ShaderVariable> &locations = location_inputs[laneIndex];
    for(const VkVertexInputAttributeDescription2EXT &attr : state.vertexAttributes)
    {
      locations.resize_for_index(attr.location);

      if(Vulkan_Debug_ShaderDebugLogging())
        NBDLOG("Populating location %u", attr.location);

      ShaderVariable &var = locations[attr.location];

      bytebuf data;

      size_t size = (size_t)GetByteSize(1, 1, 1, attr.format, 0);

      bool found = false;

      for(const VkVertexInputBindingDescription2EXT &bind : state.vertexBindings)
      {
        if(bind.binding != attr.binding)
          continue;

        if(bind.binding < state.vbuffers.size())
        {
          const VulkanRenderState::VertBuffer &vb = state.vbuffers[bind.binding];

          if(vb.buf != ResourceId())
          {
            VkDeviceSize vertexOffset = 0;

            found = true;

            if(bind.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
            {
              if(bind.divisor == 0)
                vertexOffset = instOffset * vb.stride;
              else
                vertexOffset = (instOffset + (instid / bind.divisor)) * vb.stride;
            }
            else
            {
              vertexOffset = (idx + vertOffset) * vb.stride;
            }

            if(Vulkan_Debug_ShaderDebugLogging())
            {
              NBDLOG("Fetching from %s at %llu offset %zu bytes", ToStr(vb.buf).c_str(),
                     vb.offs + attr.offset + vertexOffset, size);
            }

            if(attr.offset + vertexOffset < vb.size)
              GetDebugManager()->GetBufferData(vb.buf, vb.offs + attr.offset + vertexOffset, size,
                                               data);
          }
        }
        else if(Vulkan_Debug_ShaderDebugLogging())
        {
          NBDLOG("Vertex binding %u out of bounds from %zu vertex buffers", bind.binding,
                 state.vbuffers.size());
        }
      }

      if(!found)
      {
        if(Vulkan_Debug_ShaderDebugLogging())
        {
          NBDLOG("Attribute binding %u out of bounds from %zu bindings", attr.binding,
                 pipe.vertexBindings.size());
        }
      }

      if(size > data.size())
      {
        // out of bounds read
        m_pDriver->AddDebugMessage(
            MessageCategory::Execution, MessageSeverity::Medium, MessageSource::RuntimeWarning,
            StringFormat::Fmt(
                "Attribute location %u from binding %u reads out of bounds at vertex %u "
                "(index %u) in instance %u.",
                attr.location, attr.binding, vertid, idx, instid));

        if(IsUIntFormat(attr.format) || IsSIntFormat(attr.format))
          var.type = VarType::UInt;
        else
          var.type = VarType::Float;

        set0001(var);
      }
      else
      {
        ResourceFormat fmt = MakeResourceFormat(attr.format);

        // integer formats need to be read as-is, rather than converted to floats
        if(fmt.compType == CompType::UInt || fmt.compType == CompType::SInt)
        {
          if(fmt.type == ResourceFormatType::R10G10B10A2)
          {
            // this is the only packed UINT format
            Vec4u decoded = ConvertFromR10G10B10A2UInt(*(uint32_t *)data.data());

            var.type = VarType::UInt;

            setUintComp(var, 0, decoded.x);
            setUintComp(var, 1, decoded.y);
            setUintComp(var, 2, decoded.z);
            setUintComp(var, 3, decoded.w);
          }
          else
          {
            var.type = VarType::UInt;

            if(fmt.compType == CompType::UInt)
            {
              if(fmt.compByteWidth == 1)
                var.type = VarType::UByte;
              else if(fmt.compByteWidth == 2)
                var.type = VarType::UShort;
              else if(fmt.compByteWidth == 4)
                var.type = VarType::UInt;
              else if(fmt.compByteWidth == 8)
                var.type = VarType::ULong;
            }
            else if(fmt.compType == CompType::SInt)
            {
              if(fmt.compByteWidth == 1)
                var.type = VarType::SByte;
              else if(fmt.compByteWidth == 2)
                var.type = VarType::SShort;
              else if(fmt.compByteWidth == 4)
                var.type = VarType::SInt;
              else if(fmt.compByteWidth == 8)
                var.type = VarType::SLong;
            }

            NBDASSERTEQUAL(fmt.compByteWidth, VarTypeByteSize(var.type));
            memcpy(var.value.u8v.data(), data.data(), fmt.compByteWidth * fmt.compCount);
          }
        }
        else
        {
          FloatVector decoded = DecodeFormattedComponents(fmt, data.data());

          var.type = VarType::Float;

          setFloatComp(var, 0, decoded.x);
          setFloatComp(var, 1, decoded.y);
          setFloatComp(var, 2, decoded.z);
          setFloatComp(var, 3, decoded.w);
        }
      }
    }

    nbdspv::Debugger *debugger = new nbdspv::Debugger;
    debugger->Parse(shader.spirv.GetSPIRV());

    global_builtins[ShaderBuiltin::SubgroupSize] = ShaderVariable(nbdstr(), numThreads, 0U, 0U, 0U);

    apiWrapper->SetInputVarsToReadOnly();
    ShaderDebugTrace *ret = debugger->BeginDebug(apiWrapper, ShaderStage::Vertex, entryPoint, spec,
                                                 shadRefl.instructionLines, shadRefl.patchData,
                                                 laneIndex, numThreads, numThreads);
    apiWrapper->ResetReplay();

    return ret;
  }
}

ShaderDebugTrace *VulkanReplay::DebugPixel(uint32_t eventId, uint32_t x, uint32_t y,
                                           const DebugPixelInputs &inputs)
{
  if(!m_pDriver->GetDeviceEnabledFeatures().fragmentStoresAndAtomics)
  {
    NBDWARN("Pixel debugging is not supported without fragment stores");
    return new ShaderDebugTrace;
  }

  uint32_t sample = inputs.sample;
  uint32_t primitive = inputs.primitive;
  uint32_t view = inputs.view;

  const VulkanRenderState &state = m_pDriver->GetRenderState();
  VulkanCreationInfo &c = m_pDriver->m_CreationInfo;

  nbdstr regionName = StringFormat::Fmt("DebugPixel @ %u of (%u,%u) sample %u primitive %u view %u",
                                        eventId, x, y, sample, primitive, view);

  VkMarkerRegion region(regionName);

  if(Vulkan_Debug_ShaderDebugLogging())
    NBDLOG("%s", regionName.c_str());

  const ActionDescription *action = m_pDriver->GetAction(eventId);

  if(!(action->flags & (ActionFlags::MeshDispatch | ActionFlags::Drawcall)))
  {
    NBDLOG("No drawcall selected");
    return new ShaderDebugTrace();
  }

  const VulkanCreationInfo::Pipeline &pipe = c.m_Pipeline[state.graphics.pipeline];
  ResourceId fragId = state.graphics.shaderObject ? state.shaderObjects[4] : pipe.shaders[4].module;

  if(fragId == ResourceId())
  {
    NBDLOG("No pixel shader bound at draw");
    return new ShaderDebugTrace();
  }

  // get ourselves in pristine state before this action (without any side effects it may have had)
  m_pDriver->ReplayLog(0, eventId, eReplay_WithoutDraw);

  const VulkanCreationInfo::ShaderEntry &fragEntry =
      state.graphics.shaderObject ? c.m_ShaderObject[state.shaderObjects[4]].shad : pipe.shaders[4];
  VulkanCreationInfo::ShaderModule &shader = c.m_ShaderModule[fragEntry.module];
  nbdstr entryPoint = fragEntry.entryPoint;
  const nbdarray<SpecConstant> &spec = fragEntry.specialization;

  VulkanCreationInfo::ShaderModuleReflection &shadRefl =
      shader.GetReflection(ShaderStage::Pixel, entryPoint, state.graphics.pipeline);

  if(!shadRefl.refl->debugInfo.debuggable)
  {
    NBDLOG("Shader is not debuggable: %s", shadRefl.refl->debugInfo.debugStatus.c_str());
    return new ShaderDebugTrace();
  }

  shadRefl.PopulateDisassembly(shader.spirv);

  VulkanAPIWrapper *apiWrapper =
      new VulkanAPIWrapper(m_pDriver, c, ShaderStage::Pixel, eventId, shadRefl.refl->resourceId);

  SubgroupCapability subgroupCapability = SubgroupCapability::None;
  uint32_t maxSubgroupSize = 1;
  CalculateSubgroupProperties(maxSubgroupSize, subgroupCapability);

  uint32_t numThreads = 4;

  if(shadRefl.patchData.threadScope & nbdspv::ThreadScope::Subgroup)
    numThreads = NBDMAX(numThreads, maxSubgroupSize);

  std::unordered_map<ShaderBuiltin, ShaderVariable> &global_builtins =
      apiWrapper->GetGlobalBuiltins();
  global_builtins[ShaderBuiltin::DeviceIndex] = ShaderVariable(nbdstr(), 0U, 0U, 0U, 0U);
  global_builtins[ShaderBuiltin::DrawIndex] = ShaderVariable(nbdstr(), action->drawIndex, 0U, 0U, 0U);

  // If the pipe contains a geometry shader, then Primitive ID cannot be used in the pixel
  // shader without being emitted from the geometry shader. For now, check if this semantic
  // will succeed in a new pixel shader with the rest of the pipe unchanged
  bool usePrimitiveID = false;

  ShaderStage prevStage = ShaderStage::Geometry;

  ResourceId prevId = state.graphics.shaderObject ? state.shaderObjects[(size_t)prevStage]
                                                  : pipe.shaders[(size_t)prevStage].module;

  if(prevId == ResourceId())
  {
    prevStage = ShaderStage::Mesh;
    prevId = state.graphics.shaderObject ? state.shaderObjects[(size_t)prevStage]
                                         : pipe.shaders[(size_t)prevStage].module;
  }

  if(prevId != ResourceId())
  {
    const VulkanCreationInfo::ShaderEntry &prevEntry =
        state.graphics.shaderObject ? c.m_ShaderObject[state.shaderObjects[(size_t)prevStage]].shad
                                    : pipe.shaders[(size_t)prevStage];
    VulkanCreationInfo::ShaderModuleReflection &prevRefl =
        c.m_ShaderModule[prevEntry.module].GetReflection(prevStage, prevEntry.entryPoint,
                                                         state.graphics.pipeline);

    // check to see if the shader outputs a primitive ID
    for(const SigParameter &e : prevRefl.refl->outputSignature)
    {
      if(e.systemValue == ShaderBuiltin::PrimitiveIndex)
      {
        if(Vulkan_Debug_ShaderDebugLogging())
        {
          NBDLOG("Geometry/mesh shader exports primitive ID, can use");
        }

        usePrimitiveID = true;
        break;
      }
    }

    if(Vulkan_Debug_ShaderDebugLogging())
    {
      if(!usePrimitiveID)
        NBDLOG("Geometry/mesh shader doesn't export primitive ID, can't use");
    }
  }
  else
  {
    // no geometry shader - safe to use as long as the geometry shader capability is available
    usePrimitiveID = m_pDriver->GetDeviceEnabledFeatures().geometryShader != VK_FALSE;

    if(Vulkan_Debug_ShaderDebugLogging())
    {
      NBDLOG("usePrimitiveID is %u because of bare capability", usePrimitiveID);
    }
  }

  bool useSampleID = m_pDriver->GetDeviceEnabledFeatures().sampleRateShading != VK_FALSE;

  if(Vulkan_Debug_ShaderDebugLogging())
  {
    NBDLOG("useSampleID is %u because of bare capability", useSampleID);
  }

  // don't fetch sample ID if it would interfere with fragment rate fetch and wouldn't be needed
  // probably we could disable this entirely if MSAA is not in use, but this is the case that has
  // side effects as it disabled the VRS rates
  if(state.rastSamples == VK_SAMPLE_COUNT_1_BIT)
  {
    for(size_t i = 0; i < shadRefl.refl->inputSignature.size(); i++)
    {
      if(shadRefl.refl->inputSignature[i].systemValue == ShaderBuiltin::PackedFragRate)
      {
        useSampleID = false;
        break;
      }
    }
  }

  bool useViewIndex = (view == ~0U) ? false : true;
  if(useViewIndex)
  {
    ResourceId rp = state.GetRenderPass();
    if(rp != ResourceId())
    {
      const VulkanCreationInfo::RenderPass &rpInfo = GetDebugManager()->GetRenderPassInfo(rp);
      for(auto it = rpInfo.subpasses.begin(); it != rpInfo.subpasses.end(); ++it)
      {
        if(it->multiviews.isEmpty())
        {
          if(Vulkan_Debug_ShaderDebugLogging())
            NBDLOG(
                "Disabling useViewIndex because at least one subpass does not have multiple views");
          useViewIndex = false;
          break;
        }
      }
    }
    else
    {
      useViewIndex =
          (state.dynamicRendering.active ? state.dynamicRendering.viewMask : pipe.viewMask) != 0;
      if(!useViewIndex && Vulkan_Debug_ShaderDebugLogging())
        NBDLOG("Disabling useViewIndex because viewMask is zero");
    }
  }
  else
  {
    if(Vulkan_Debug_ShaderDebugLogging())
      NBDLOG("Disabling useViewIndex from input view %u", view);
  }
  if(useViewIndex)
  {
    global_builtins[ShaderBuiltin::MultiViewIndex] = ShaderVariable(nbdstr(), view, 0U, 0U, 0U);
  }

  uint32_t paramAlign, structStride;
  nbdtie(paramAlign, structStride) = GetAlignAndOutputSize(shadRefl);

  uint32_t overdrawLevels = 100;    // maximum number of overdraw levels

  // struct size is nbdspv::ResultDataBase header plus Nx structStride for the number of threads
  uint32_t structSize = sizeof(nbdspv::ResultDataBase) + structStride * numThreads;

  VkDeviceSize feedbackStorageSize = overdrawLevels * structSize + sizeof(Vec4f) + 1024;

  if(Vulkan_Debug_ShaderDebugLogging())
  {
    NBDLOG("Output structure is %u sized, output buffer is %llu bytes", structStride,
           feedbackStorageSize);
  }

  m_PatchedShaderFeedback.ResizeFeedbackBuffer(m_pDriver, feedbackStorageSize);

  SpecData specData = {};

  specData.arrayLength = overdrawLevels;
  specData.destX = float(x);
  specData.destY = float(y);

  // make copy of state to draw from
  VulkanRenderState modifiedstate = state;

  AddedDescriptorData patchedBufferdata = PrepareExtraBufferDescriptor(
      modifiedstate, false, {MakeNewBinding(VK_SHADER_STAGE_FRAGMENT_BIT)}, false);

  if(patchedBufferdata.empty())
  {
    delete apiWrapper;

    ShaderDebugTrace *ret = new ShaderDebugTrace;
    ret->stage = ShaderStage::Pixel;

    return ret;
  }

  if(!patchedBufferdata.descSets.empty())
    m_PatchedShaderFeedback.FeedbackBuffer.WriteDescriptor(Unwrap(patchedBufferdata.descSets[0]), 0,
                                                           0);

  specData.bufferAddress = m_PatchedShaderFeedback.FeedbackBuffer.Address();
  if(specData.bufferAddress && Vulkan_Debug_ShaderDebugLogging())
  {
    NBDLOG("Got buffer address of %llu", specData.bufferAddress);
  }

  // create  shader with modified code

  VkSpecializationMapEntry specMaps[(size_t)InputSpecConstant::Count];
  NBDCOMPILE_ASSERT(sizeof(specMaps) == sizeof(specMapsTemplate),
                    "Specialisation maps have changed");

  VkSpecializationInfo patchedSpecInfo = MakeSpecInfo(specData, specMaps);

  auto patchCallback = [this, &spec, &shadRefl, &patchedSpecInfo, usePrimitiveID, useSampleID,
                        useViewIndex, subgroupCapability, maxSubgroupSize](
                           const AddedDescriptorData &patchedBufferdata, VkShaderStageFlagBits stage,
                           const char *entryName, const nbdarray<uint32_t> &origSpirv,
                           nbdarray<uint32_t> &modSpirv, const VkSpecializationInfo *&specInfo) {
    if(stage != VK_SHADER_STAGE_FRAGMENT_BIT)
      return false;

    modSpirv = origSpirv;

    if(!Vulkan_Debug_PSDebugDumpDirPath().empty())
      FileIO::WriteAll(Vulkan_Debug_PSDebugDumpDirPath() + "/debug_psinput_before.spv", modSpirv);

    CreateInputFetcher(modSpirv, spec, shadRefl, m_StorageMode, usePrimitiveID, useSampleID,
                       useViewIndex, subgroupCapability, maxSubgroupSize);

    if(!Vulkan_Debug_PSDebugDumpDirPath().empty())
      FileIO::WriteAll(Vulkan_Debug_PSDebugDumpDirPath() + "/debug_psinput_after.spv", modSpirv);

    // overwrite user's specialisation info. We flattened the user's spec constants when patching the SPIR-V above.
    specInfo = &patchedSpecInfo;

    return true;
  };

  PrepareStateForPatchedShader(patchedBufferdata, modifiedstate, false, patchCallback);

  if(!RunFeedbackAction(feedbackStorageSize, action, modifiedstate))
  {
    delete apiWrapper;

    ShaderDebugTrace *ret = new ShaderDebugTrace;
    ret->stage = ShaderStage::Pixel;

    return ret;
  }

  bytebuf data;
  GetDebugManager()->GetBufferData(m_PatchedShaderFeedback.FeedbackBuffer, 0, 0, data);

  byte *base = data.data();
  uint32_t hit_count = ((uint32_t *)base)[0];
  uint32_t total_count = ((uint32_t *)base)[1];

  if(hit_count > overdrawLevels)
  {
    NBDERR("%u hits, more than max overdraw levels allowed %u. Clamping", hit_count, overdrawLevels);
    hit_count = overdrawLevels;
  }

  base += sizeof(Vec4f);

  nbdspv::ResultDataBase *winner = NULL;

  NBDLOG("Got %u hit candidates out of %u total instances", hit_count, total_count);

  // if we encounter multiple hits at our destination pixel co-ord (or any other) we
  // check to see if a specific primitive was requested (via primitive parameter not
  // being set to ~0U). If it was, debug that pixel, otherwise do a best-estimate
  // of which fragment was the last to successfully depth test and debug that, just by
  // checking if the depth test is ordered and picking the final fragment in the series

  VkCompareOp depthOp = state.depthCompareOp;

  // depth tests disabled acts the same as always compare mode
  if(!state.depthTestEnable)
    depthOp = VK_COMPARE_OP_ALWAYS;

  for(uint32_t i = 0; i < hit_count; i++)
  {
    nbdspv::ResultDataBase *hit = (nbdspv::ResultDataBase *)(base + structSize * i);

    if(hit->valid != validMagicNumber)
    {
      NBDWARN("Hit %u doesn't have valid magic number", i);
      continue;
    }

    float ddxExpectation = 1.0f;
    if(hit->shadRate & (uint32_t)nbdspv::FragmentShadingRate::Horizontal2Pixels)
      ddxExpectation = 2.0f;
    else if(hit->shadRate & (uint32_t)nbdspv::FragmentShadingRate::Horizontal4Pixels)
      ddxExpectation = 4.0f;

    if(hit->ddxDerivCheck != ddxExpectation)
    {
      NBDWARN("Hit %u doesn't have valid derivatives", i);
      continue;
    }

    // if we're looking for a specific view, ignore hits from the wrong view
    if(useViewIndex)
    {
      if(hit->view != view)
        continue;
    }

    // see if this hit is a closer match than the previous winner.

    // if there's no previous winner it's clearly better
    if(winner == NULL)
    {
      winner = hit;
      continue;
    }

    // if we're looking for a specific primitive
    if(primitive != ~0U)
    {
      // and this hit is a match and the winner isn't, it's better
      if(winner->prim != primitive && hit->prim == primitive)
      {
        winner = hit;
        continue;
      }

      // if the winner is a match and we're not, we can't be better so stop now
      if(winner->prim == primitive && hit->prim != primitive)
      {
        continue;
      }
    }

    // if we're looking for a particular sample, check that
    if(sample != ~0U)
    {
      if(winner->sample != sample && hit->sample == sample)
      {
        winner = hit;
        continue;
      }

      if(winner->sample == sample && hit->sample != sample)
      {
        continue;
      }
    }

    // otherwise apply depth test
    switch(depthOp)
    {
      case VK_COMPARE_OP_NEVER:
      case VK_COMPARE_OP_EQUAL:
      case VK_COMPARE_OP_NOT_EQUAL:
      case VK_COMPARE_OP_ALWAYS:
      default:
        // don't emulate equal or not equal since we don't know the reference value. Take any hit
        // (thus meaning the last hit)
        winner = hit;
        break;
      case VK_COMPARE_OP_LESS:
        if(hit->pos.z < winner->pos.z)
          winner = hit;
        break;
      case VK_COMPARE_OP_LESS_OR_EQUAL:
        if(hit->pos.z <= winner->pos.z)
          winner = hit;
        break;
      case VK_COMPARE_OP_GREATER:
        if(hit->pos.z > winner->pos.z)
          winner = hit;
        break;
      case VK_COMPARE_OP_GREATER_OR_EQUAL:
        if(hit->pos.z >= winner->pos.z)
          winner = hit;
        break;
    }
  }

  ShaderDebugTrace *ret = NULL;

  if(winner)
  {
    nbdspv::Debugger *debugger = new nbdspv::Debugger;
    debugger->Parse(shader.spirv.GetSPIRV());

    // the per-thread data immediately follows the nbdspv::ResultDataBase header. Every piece of
    // data is uniformly aligned, either 16-byte by default or 32-byte if larger components exist.
    // The output is in input signature order.
    byte *LaneData = (byte *)(winner + 1);

    numThreads = 4;

    if(shadRefl.patchData.threadScope & nbdspv::ThreadScope::Subgroup)
    {
      NBDASSERTNOTEQUAL(winner->subgroupSize, 0);
      numThreads = NBDMAX(numThreads, winner->subgroupSize);
    }

    nbdarray<nbdarray<ShaderVariable>> &location_inputs = apiWrapper->GetLocationInputs();
    nbdarray<std::unordered_map<ShaderBuiltin, ShaderVariable>> &allthread_builtins =
        apiWrapper->GetThreadBuiltins();
    location_inputs.resize(numThreads);
    allthread_builtins.resize(numThreads);
    apiWrapper->thread_props.resize(numThreads);

    for(uint32_t t = 0; t < numThreads; t++)
    {
      byte *value = LaneData + t * structStride;

      if(shadRefl.patchData.threadScope & nbdspv::ThreadScope::Subgroup)
      {
        nbdspv::SubgroupLaneData *subgroupData = (nbdspv::SubgroupLaneData *)value;
        apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::Active] = subgroupData->isActive;
        apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::Elected] = subgroupData->elect;
        apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::SubgroupId] = t;

        value += sizeof(nbdspv::SubgroupLaneData);
      }

      // read PixelLaneData
      {
        nbdspv::PixelLaneData *pixelData = (nbdspv::PixelLaneData *)value;

        {
          ShaderVariable &var = allthread_builtins[t][ShaderBuiltin::Position];

          var.rows = 1;
          var.columns = 4;
          var.type = VarType::Float;

          memcpy(var.value.u8v.data(), &pixelData->fragCoord, sizeof(Vec4f));
        }

        {
          ShaderVariable &var = allthread_builtins[t][ShaderBuiltin::IsHelper];

          var.rows = 1;
          var.columns = 1;
          var.type = VarType::Bool;

          memcpy(var.value.u8v.data(), &pixelData->isHelper, sizeof(uint32_t));
        }

        if(numThreads == 4)
          apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::Active] = 1;
        apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::Helper] = pixelData->isHelper;
        apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::QuadId] = pixelData->quadId;
        apiWrapper->thread_props[t][(size_t)nbdspv::ThreadProperty::QuadLane] =
            pixelData->quadLaneIndex;
      }
      value += sizeof(nbdspv::PixelLaneData);

      for(size_t i = 0; i < shadRefl.refl->inputSignature.size(); i++)
      {
        const SigParameter &param = shadRefl.refl->inputSignature[i];

        bool builtin = true;
        if(param.systemValue == ShaderBuiltin::Undefined)
        {
          builtin = false;
          location_inputs[t].resize(NBDMAX((uint32_t)location_inputs.size(), param.regIndex + 1));
        }

        ShaderVariable &var =
            builtin ? allthread_builtins[t][param.systemValue] : location_inputs[t][param.regIndex];

        var.rows = 1;
        var.columns = param.compCount & 0xff;
        var.type = param.varType;

        const uint32_t firstComp = Bits::CountTrailingZeroes(uint32_t(param.regChannelMask));
        const uint32_t elemSize = VarTypeByteSize(param.varType);

        // we always store in 32-bit types
        const size_t sz = NBDMAX(4U, elemSize) * param.compCount;

        memcpy((var.value.u8v.data()) + elemSize * firstComp, value + i * paramAlign, sz);

        // convert down from stored 32-bit types if they were smaller
        if(elemSize == 1)
        {
          ShaderVariable tmp = var;

          for(uint32_t comp = 0; comp < param.compCount; comp++)
            var.value.u8v[comp] = tmp.value.u32v[comp] & 0xff;
        }
        else if(elemSize == 2)
        {
          ShaderVariable tmp = var;

          for(uint32_t comp = 0; comp < param.compCount; comp++)
          {
            if(VarTypeCompType(param.varType) == CompType::Float)
              var.value.f16v[comp] = rdhalf::make(tmp.value.f32v[comp]);
            else
              var.value.u16v[comp] = tmp.value.u32v[comp] & 0xffff;
          }
        }
      }
    }

    global_builtins[ShaderBuiltin::SubgroupSize] = ShaderVariable(nbdstr(), numThreads, 0U, 0U, 0U);

    apiWrapper->SetInputVarsToReadOnly();
    ret = debugger->BeginDebug(apiWrapper, ShaderStage::Pixel, entryPoint, spec,
                               shadRefl.instructionLines, shadRefl.patchData, winner->laneIndex,
                               numThreads, numThreads);
    apiWrapper->ResetReplay();
  }
  else
  {
    NBDLOG("Didn't get any valid hit to debug");
    delete apiWrapper;

    ret = new ShaderDebugTrace;
    ret->stage = ShaderStage::Pixel;
  }

  patchedBufferdata.Free();
  return ret;
}

ShaderDebugTrace *VulkanReplay::DebugThread(uint32_t eventId,
                                            const nbdfixedarray<uint32_t, 3> &groupid,
                                            const nbdfixedarray<uint32_t, 3> &threadid)
{
  return DebugComputeCommon(ShaderStage::Compute, eventId, groupid, threadid);
}

ShaderDebugTrace *VulkanReplay::DebugMeshThread(uint32_t eventId,
                                                const nbdfixedarray<uint32_t, 3> &groupid,
                                                const nbdfixedarray<uint32_t, 3> &threadid)
{
  return DebugComputeCommon(ShaderStage::Mesh, eventId, groupid, threadid);
}

ShaderDebugTrace *VulkanReplay::DebugComputeCommon(ShaderStage stage, uint32_t eventId,
                                                   const nbdfixedarray<uint32_t, 3> &groupid,
                                                   const nbdfixedarray<uint32_t, 3> &threadid)
{
  const VulkanRenderState &state = m_pDriver->GetRenderState();
  VulkanCreationInfo &c = m_pDriver->m_CreationInfo;

  nbdstr regionName =
      StringFormat::Fmt("Debug %s @ %u of (%u,%u,%u) (%u,%u,%u)", ToStr(stage).c_str(), eventId,
                        groupid[0], groupid[1], groupid[2], threadid[0], threadid[1], threadid[2]);

  VkMarkerRegion region(regionName);

  if(Vulkan_Debug_ShaderDebugLogging())
    NBDLOG("%s", regionName.c_str());

  const ActionDescription *action = m_pDriver->GetAction(eventId);

  if(stage == ShaderStage::Compute)
  {
    if(!(action->flags & ActionFlags::Dispatch))
    {
      NBDLOG("No dispatch selected");
      return new ShaderDebugTrace();
    }
  }
  else
  {
    if(!(action->flags & ActionFlags::MeshDispatch))
    {
      NBDLOG("No dispatch selected");
      return new ShaderDebugTrace();
    }
  }

  // get ourselves in pristine state before this dispatch (without any side effects it may have had)
  m_pDriver->ReplayLog(0, eventId, eReplay_WithoutDraw);

  const VulkanStatePipeline &stagePipeState =
      stage == ShaderStage::Compute ? state.compute : state.graphics;
  const VulkanCreationInfo::Pipeline &pipe = c.m_Pipeline[stagePipeState.pipeline];
  const VulkanCreationInfo::ShaderEntry &shaderEntry =
      stagePipeState.shaderObject ? c.m_ShaderObject[state.shaderObjects[(size_t)stage]].shad
                                  : pipe.shaders[(size_t)stage];
  VulkanCreationInfo::ShaderModule &shader = c.m_ShaderModule[shaderEntry.module];
  nbdstr entryPoint = shaderEntry.entryPoint;
  const nbdarray<SpecConstant> &spec = shaderEntry.specialization;

  VulkanCreationInfo::ShaderModuleReflection &shadRefl =
      shader.GetReflection(stage, entryPoint, stagePipeState.pipeline);

  if(!shadRefl.refl->debugInfo.debuggable)
  {
    NBDLOG("Shader is not debuggable: %s", shadRefl.refl->debugInfo.debugStatus.c_str());
    return new ShaderDebugTrace();
  }

  shadRefl.PopulateDisassembly(shader.spirv);

  VulkanAPIWrapper *apiWrapper =
      new VulkanAPIWrapper(m_pDriver, c, stage, eventId, shadRefl.refl->resourceId);

  uint32_t threadDim[3];
  threadDim[0] = shadRefl.refl->dispatchThreadsDimension[0];
  threadDim[1] = shadRefl.refl->dispatchThreadsDimension[1];
  threadDim[2] = shadRefl.refl->dispatchThreadsDimension[2];

  if((threadid[0] >= threadDim[0]) || (threadid[1] >= threadDim[1]) || (threadid[2] >= threadDim[2]))
  {
    NBDLOG("Invalid threadid %d,%d,%d selected from group %dx%dx%d", threadid[0], threadid[1],
           threadid[2], threadDim[0], threadDim[1], threadDim[2]);
    return new ShaderDebugTrace();
  }
  if((groupid[0] >= action->dispatchDimension[0]) || (groupid[1] >= action->dispatchDimension[1]) ||
     (groupid[2] >= action->dispatchDimension[2]))
  {
    NBDLOG("Invalid groupid %d,%d,%d selected from dispatch %dx%dx%d", groupid[0], groupid[1],
           groupid[2], action->dispatchDimension[0], action->dispatchDimension[1],
           action->dispatchDimension[2]);
    return new ShaderDebugTrace();
  }

  SubgroupCapability subgroupCapability = SubgroupCapability::None;
  uint32_t maxSubgroupSize = 1;
  CalculateSubgroupProperties(maxSubgroupSize, subgroupCapability);

  uint32_t numThreads = 1;

  bool hasQuadScope = (shadRefl.patchData.threadScope & nbdspv::ThreadScope::Quad) ? true : false;
  bool hasQuadDerivatives =
      (shadRefl.patchData.derivativeMode != nbdspv::ComputeDerivativeMode::None);
  bool hasSubgroupScoope =
      (shadRefl.patchData.threadScope & nbdspv::ThreadScope::Subgroup) ? true : false;
  bool hasWorkgroupScope =
      (shadRefl.patchData.threadScope & nbdspv::ThreadScope::Workgroup) ? true : false;

  if(hasQuadDerivatives || hasQuadScope)
    numThreads = NBDMAX(numThreads, 4U);
  if(hasSubgroupScoope)
    numThreads = NBDMAX(numThreads, maxSubgroupSize);
  if(hasWorkgroupScope)
    numThreads = NBDMAX(numThreads, threadDim[0] * threadDim[1] * threadDim[2]);

  nbdarray<std::unordered_map<ShaderBuiltin, ShaderVariable>> &allthread_builtins =
      apiWrapper->GetThreadBuiltins();
  allthread_builtins.resize(numThreads);
  apiWrapper->thread_props.resize(numThreads);

  std::unordered_map<ShaderBuiltin, ShaderVariable> &global_builtins =
      apiWrapper->GetGlobalBuiltins();
  global_builtins[ShaderBuiltin::DispatchSize] =
      ShaderVariable(nbdstr(), action->dispatchDimension[0], action->dispatchDimension[1],
                     action->dispatchDimension[2], 0U);
  global_builtins[ShaderBuiltin::GroupSize] =
      ShaderVariable(nbdstr(), threadDim[0], threadDim[1], threadDim[2], 0U);
  global_builtins[ShaderBuiltin::DeviceIndex] = ShaderVariable(nbdstr(), 0U, 0U, 0U, 0U);
  global_builtins[ShaderBuiltin::GroupIndex] =
      ShaderVariable(nbdstr(), groupid[0], groupid[1], groupid[2], 0U);

  const uint32_t quadIdOffset = 10000;
  const uint32_t quadDerivMode = (uint32_t)shadRefl.patchData.derivativeMode;

  uint32_t countQuadX = ~0U;
  uint32_t countQuadY = ~0U;
  uint32_t quadW = ~0U;
  uint32_t quadH = ~0U;

  if(hasQuadDerivatives)
  {
    // linear: 4x1x1
    // quad: 2x2x1
    const uint32_t quadWidths[3] = {~0U, 4, 2};
    const uint32_t quadHeights[3] = {~0U, 1, 2};
    quadW = quadWidths[quadDerivMode];
    quadH = quadHeights[quadDerivMode];
    countQuadX = threadDim[0] / quadW;
    countQuadY = threadDim[1] / quadH;
    hasQuadScope = true;
  }
  else if(hasQuadScope)
  {
    // Choose linear layout
    quadW = 4;
    quadH = 1;
    countQuadX = threadDim[0] / quadW;
    countQuadY = threadDim[1] / quadH;
  }

  if(hasQuadScope)
  {
    NBDASSERTEQUAL(threadDim[0], countQuadX * quadW);
    NBDASSERTEQUAL(threadDim[1], countQuadY * quadH);
  }

  // if we need to fetch subgroup data, do that now
  uint32_t laneIndex = 0;
  if(hasSubgroupScoope)
  {
    SpecData specData = {};

    specData.globalThreadIdX = groupid[0] * threadDim[0] + threadid[0];
    specData.globalThreadIdY = groupid[1] * threadDim[1] + threadid[1];
    specData.globalThreadIdZ = groupid[2] * threadDim[2] + threadid[2];

    uint32_t paramAlign, structStride;
    nbdtie(paramAlign, structStride) = GetAlignAndOutputSize(shadRefl);

    uint32_t maxHits = 4;    // we should only ever get one hit

    // struct size is nbdspv::ResultDataBase header plus Nx structStride for the number of threads
    uint32_t structSize = sizeof(nbdspv::ResultDataBase) + structStride * maxSubgroupSize;

    VkDeviceSize feedbackStorageSize = maxHits * structSize + 1024;

    if(Vulkan_Debug_ShaderDebugLogging())
    {
      NBDLOG("Output structure is %u sized, output buffer is %llu bytes", structStride,
             feedbackStorageSize);
    }

    m_PatchedShaderFeedback.ResizeFeedbackBuffer(m_pDriver, feedbackStorageSize);

    specData.arrayLength = maxHits;

    // make copy of state to draw from
    VulkanRenderState modifiedstate = state;

    VkShaderStageFlagBits stageBit = (VkShaderStageFlagBits)ShaderMaskFromIndex(shadRefl.stageIndex);
    AddedDescriptorData patchedBufferdata = PrepareExtraBufferDescriptor(
        modifiedstate, stage == ShaderStage::Compute, {MakeNewBinding(stageBit)}, false);

    if(patchedBufferdata.empty())
    {
      delete apiWrapper;

      ShaderDebugTrace *ret = new ShaderDebugTrace;
      ret->stage = stage;

      return ret;
    }

    if(!patchedBufferdata.descSets.empty())
      m_PatchedShaderFeedback.FeedbackBuffer.WriteDescriptor(Unwrap(patchedBufferdata.descSets[0]),
                                                             0, 0);

    specData.bufferAddress = m_PatchedShaderFeedback.FeedbackBuffer.Address();
    if(specData.bufferAddress && Vulkan_Debug_ShaderDebugLogging())
    {
      NBDLOG("Got buffer address of %llu", specData.bufferAddress);
    }

    // create shader with modified code

    VkSpecializationMapEntry specMaps[(size_t)InputSpecConstant::Count];
    NBDCOMPILE_ASSERT(sizeof(specMaps) == sizeof(specMapsTemplate),
                      "Specialisation maps have changed");

    VkSpecializationInfo patchedSpecInfo = MakeSpecInfo(specData, specMaps);

    auto patchCallback = [this, stageBit, &spec, &shadRefl, &patchedSpecInfo, subgroupCapability,
                          maxSubgroupSize](const AddedDescriptorData &patchedBufferdata,
                                           VkShaderStageFlagBits stage, const char *entryName,
                                           const nbdarray<uint32_t> &origSpirv,
                                           nbdarray<uint32_t> &modSpirv,
                                           const VkSpecializationInfo *&specInfo) {
      if(stage != stageBit)
        return false;

      modSpirv = origSpirv;

      uint32_t idx = shadRefl.stageIndex;

      static const nbdstr filename[NumShaderStages] = {
          "shadinput_vertex.spv",   "shadinput_hull.spv",  "shadinput_domain.spv",
          "shadinput_geometry.spv", "shadinput_pixel.spv", "shadinput_compute.spv",
          "shadinput_task.spv",     "shadinput_mesh.spv",
      };

      if(!Vulkan_Debug_PSDebugDumpDirPath().empty())
        FileIO::WriteAll(Vulkan_Debug_PSDebugDumpDirPath() + "/before_" + filename[idx], modSpirv);

      CreateInputFetcher(modSpirv, spec, shadRefl, m_StorageMode, false, false, false,
                         subgroupCapability, maxSubgroupSize);

      if(!Vulkan_Debug_PSDebugDumpDirPath().empty())
        FileIO::WriteAll(Vulkan_Debug_PSDebugDumpDirPath() + "/after_" + filename[idx], modSpirv);

      // overwrite user's specialisation info. We flattened the user's spec constants when patching the SPIR-V above.
      specInfo = &patchedSpecInfo;

      return true;
    };

    PrepareStateForPatchedShader(patchedBufferdata, modifiedstate, stage == ShaderStage::Compute,
                                 patchCallback);

    if(!RunFeedbackAction(feedbackStorageSize, action, modifiedstate))
    {
      delete apiWrapper;

      ShaderDebugTrace *ret = new ShaderDebugTrace;
      ret->stage = stage;

      return ret;
    }

    bytebuf data;
    GetDebugManager()->GetBufferData(m_PatchedShaderFeedback.FeedbackBuffer, 0, 0, data);

    byte *base = data.data();
    uint32_t hit_count = ((uint32_t *)base)[0];
    // uint32_t total_count = ((uint32_t *)base)[1];

    if(hit_count > maxHits)
    {
      NBDERR("%u hits, more than max overdraw levels allowed %u. Clamping", hit_count, maxHits);
      hit_count = maxHits;
    }

    base += sizeof(Vec4f);

    nbdspv::ResultDataBase *winner = (nbdspv::ResultDataBase *)base;

    if(winner->valid != validMagicNumber)
    {
      NBDWARN("Hit doesn't have valid magic number");

      delete apiWrapper;

      ShaderDebugTrace *ret = new ShaderDebugTrace;
      ret->stage = stage;

      return ret;
    }

    nbdspv::Debugger *debugger = new nbdspv::Debugger;
    debugger->Parse(shader.spirv.GetSPIRV());

    // the per-thread data immediately follows the nbdspv::ResultDataBase header. Every piece of
    // data is uniformly aligned, either 16-byte by default or 32-byte if larger components exist.
    // The output is in input signature order.
    byte *LaneData = (byte *)(winner + 1);

    const uint32_t subgroupSize = winner->subgroupSize;

    NBDASSERTNOTEQUAL(subgroupSize, 0);
    numThreads = NBDMAX(numThreads, subgroupSize);

    if(hasWorkgroupScope)
      numThreads = NBDMAX(numThreads, threadDim[0] * threadDim[1] * threadDim[2]);

    if(hasQuadScope)
      NBDASSERT(numThreads >= 4);

    global_builtins[ShaderBuiltin::NumSubgroups] =
        ShaderVariable(nbdstr(), winner->numSubgroups, 0U, 0U, 0U);

    apiWrapper->thread_props.resize(numThreads);

    laneIndex = ~0U;

    for(uint32_t t = 0; t < subgroupSize; t++)
    {
      byte *value = LaneData + t * structStride;

      nbdspv::SubgroupLaneData *subgroupData = (nbdspv::SubgroupLaneData *)value;
      value += sizeof(nbdspv::SubgroupLaneData);

      nbdspv::ComputeLaneData *compData = (nbdspv::ComputeLaneData *)value;
      value += sizeof(nbdspv::ComputeLaneData);

      uint32_t lane = t;

      uint32_t quadId = ~0U;
      uint32_t quadLaneIndex = ~0U;
      if(hasQuadScope)
      {
        uint32_t quadX = (compData->threadid[0] / quadW);
        uint32_t quadY = (compData->threadid[1] / quadH);
        uint32_t quadZ = compData->threadid[2];
        quadId = quadX + (quadY * countQuadX) + (quadZ * countQuadY * countQuadX);
        quadLaneIndex = (compData->threadid[0] % quadW) + (compData->threadid[1] % quadH) * 2;
      }

      if(hasWorkgroupScope)
      {
        if(hasQuadScope)
        {
          // There won't be any data them for inactive lanes
          if(subgroupData->isActive == 0)
            continue;

          // quad scope, derive the lane from the quad layout
          lane = quadId * 4 + quadLaneIndex;
        }
        else
        {
          // Assume linear layout for the subgroup : tightly wrapped
          lane = compData->threadid[2] * threadDim[0] * threadDim[1] +
                 compData->threadid[1] * threadDim[0] + compData->threadid[0];
        }
      }

      if(hasQuadScope)
      {
        apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::QuadLane] = quadLaneIndex;
        apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::QuadId] =
            quadId + quadIdOffset;
      }

      if(nbdfixedarray<uint32_t, 3>(compData->threadid) == threadid && subgroupData->isActive)
        laneIndex = lane;

      apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::Active] = subgroupData->isActive;
      apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::Elected] = subgroupData->elect;
      apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::SubgroupId] = t;

      allthread_builtins[lane][ShaderBuiltin::DispatchThreadIndex] =
          ShaderVariable(nbdstr(), groupid[0] * threadDim[0] + compData->threadid[0],
                         groupid[1] * threadDim[1] + compData->threadid[1],
                         groupid[2] * threadDim[2] + compData->threadid[2], 0U);
      allthread_builtins[lane][ShaderBuiltin::GroupThreadIndex] = ShaderVariable(
          nbdstr(), compData->threadid[0], compData->threadid[1], compData->threadid[2], 0U);
      allthread_builtins[lane][ShaderBuiltin::GroupFlatIndex] =
          ShaderVariable(nbdstr(),
                         compData->threadid[2] * threadDim[0] * threadDim[1] +
                             compData->threadid[1] * threadDim[0] + compData->threadid[0],
                         0U, 0U, 0U);
      allthread_builtins[lane][ShaderBuiltin::IndexInSubgroup] =
          ShaderVariable(nbdstr(), t, 0U, 0U, 0U);
      allthread_builtins[lane][ShaderBuiltin::SubgroupIndexInWorkgroup] =
          ShaderVariable(nbdstr(), compData->subIdxInGroup, 0U, 0U, 0U);
    }

    if(laneIndex == ~0U)
    {
      NBDERR("Didn't find desired lane in subgroup data");
      laneIndex = 0;
    }

    // if we're simulating the whole workgroup we need to fill in the thread IDs of other threads
    if(hasWorkgroupScope)
    {
      for(uint32_t tz = 0; tz < threadDim[2]; tz++)
      {
        for(uint32_t ty = 0; ty < threadDim[1]; ty++)
        {
          for(uint32_t tx = 0; tx < threadDim[0]; tx++)
          {
            uint32_t quadId = ~0U;
            uint32_t quadLaneIndex = ~0U;

            uint32_t lane = ~0U;
            if(hasQuadScope)
            {
              // quad scope, derive the lane from the quad layout
              uint32_t quadX = (tx / quadW);
              uint32_t quadY = (ty / quadH);
              uint32_t quadZ = tz;
              quadId = quadX + (quadY * countQuadX) + (quadZ * countQuadY * countQuadX);
              quadLaneIndex = (tx % quadW) + (ty % quadH) * 2;
              lane = quadId * 4 + quadLaneIndex;
            }
            else
            {
              // Assume linear layout for the subgroup : tightly wrapped
              lane = tz * threadDim[0] * threadDim[1] + ty * threadDim[0] + tx;
            }
            std::unordered_map<ShaderBuiltin, ShaderVariable> &thread_builtins =
                allthread_builtins[lane];

            thread_builtins[ShaderBuiltin::GroupThreadIndex] =
                ShaderVariable(nbdstr(), tx, ty, tz, 0U);
            thread_builtins[ShaderBuiltin::GroupFlatIndex] = ShaderVariable(
                nbdstr(), tz * threadDim[0] * threadDim[1] + ty * threadDim[0] + tx, 0U, 0U, 0U);

            if(apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::Active])
            {
              // assert that this is the thread we expect it to be
              NBDASSERTEQUAL(thread_builtins[ShaderBuiltin::DispatchThreadIndex].value.u32v[0],
                             groupid[0] * threadDim[0] + tx);
              NBDASSERTEQUAL(thread_builtins[ShaderBuiltin::DispatchThreadIndex].value.u32v[1],
                             groupid[1] * threadDim[1] + ty);
              NBDASSERTEQUAL(thread_builtins[ShaderBuiltin::DispatchThreadIndex].value.u32v[2],
                             groupid[2] * threadDim[2] + tz);

              NBDASSERTEQUAL(thread_builtins[ShaderBuiltin::IndexInSubgroup].value.u32v[0],
                             lane % subgroupSize);
              NBDASSERTEQUAL(thread_builtins[ShaderBuiltin::SubgroupIndexInWorkgroup].value.u32v[0],
                             lane / subgroupSize);

              if(hasQuadScope)
              {
                NBDASSERTEQUAL(
                    apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::QuadLane],
                    quadLaneIndex);
                NBDASSERTEQUAL(apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::QuadId],
                               quadId + quadIdOffset);
              }
            }
            else
            {
              thread_builtins[ShaderBuiltin::DispatchThreadIndex] =
                  ShaderVariable(nbdstr(), groupid[0] * threadDim[0] + tx,
                                 groupid[1] * threadDim[1] + ty, groupid[2] * threadDim[2] + tz, 0U);
              // tightly wrap subgroups, this is likely not how the GPU actually assigns them
              thread_builtins[ShaderBuiltin::IndexInSubgroup] =
                  ShaderVariable(nbdstr(), lane % subgroupSize, 0U, 0U, 0U);
              thread_builtins[ShaderBuiltin::SubgroupIndexInWorkgroup] =
                  ShaderVariable(nbdstr(), lane / subgroupSize, 0U, 0U, 0U);
              apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::Active] = 1;
              apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::SubgroupId] =
                  lane % subgroupSize;

              if(hasQuadScope)
              {
                apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::QuadLane] =
                    quadLaneIndex;
                apiWrapper->thread_props[lane][(size_t)nbdspv::ThreadProperty::QuadId] = quadId;
              }
            }
          }
        }
      }
    }

    // Each member of a quad should belong to the same subgroup. We assume this and do not validate it

    // Add inactive padding lanes to round up to the subgroup size
    const uint32_t numPaddingThreads = AlignUp(numThreads, subgroupSize) - numThreads;
    if(numPaddingThreads > 0)
    {
      uint32_t newNumThreads = numThreads + numPaddingThreads;
      apiWrapper->thread_props.resize(newNumThreads);
      allthread_builtins.resize(newNumThreads);
      for(uint32_t i = numThreads; i < newNumThreads; ++i)
      {
        std::unordered_map<ShaderBuiltin, ShaderVariable> &thread_builtins = allthread_builtins[i];

        thread_builtins[ShaderBuiltin::DispatchThreadIndex] =
            ShaderVariable(nbdstr(), -1, -1, -1, -1);
        thread_builtins[ShaderBuiltin::GroupThreadIndex] = ShaderVariable(nbdstr(), -1, -1, -1, -1);
        thread_builtins[ShaderBuiltin::GroupFlatIndex] = ShaderVariable(nbdstr(), -1, -1, -1, -1);
        thread_builtins[ShaderBuiltin::IndexInSubgroup] =
            ShaderVariable(nbdstr(), i % subgroupSize, 0U, 0U, 0U);
        thread_builtins[ShaderBuiltin::SubgroupIndexInWorkgroup] =
            ShaderVariable(nbdstr(), i / subgroupSize, 0U, 0U, 0U);
        apiWrapper->thread_props[i][(size_t)nbdspv::ThreadProperty::Active] = 0;
        apiWrapper->thread_props[i][(size_t)nbdspv::ThreadProperty::SubgroupId] = i % subgroupSize;
      }
      numThreads = newNumThreads;
    }
    global_builtins[ShaderBuiltin::SubgroupSize] = ShaderVariable(nbdstr(), subgroupSize, 0U, 0U, 0U);

    apiWrapper->SetInputVarsToReadOnly();
    ShaderDebugTrace *ret =
        debugger->BeginDebug(apiWrapper, stage, entryPoint, spec, shadRefl.instructionLines,
                             shadRefl.patchData, laneIndex, numThreads, subgroupSize);
    apiWrapper->ResetReplay();

    return ret;
  }
  else
  {
    // if we need to simulate the whole workgroup.
    // we assume the layout of this is irrelevant and don't attempt to read it back from the GPU
    // like we do with subgroups. We lay things out in plain linear order, along X and then Y and
    // then Z, with groups iterated together.
    if(hasWorkgroupScope)
    {
      uint32_t i = 0;
      for(uint32_t tz = 0; tz < threadDim[2]; tz++)
      {
        for(uint32_t ty = 0; ty < threadDim[1]; ty++)
        {
          for(uint32_t tx = 0; tx < threadDim[0]; tx++)
          {
            std::unordered_map<ShaderBuiltin, ShaderVariable> &thread_builtins =
                allthread_builtins[i];
            thread_builtins[ShaderBuiltin::DispatchThreadIndex] =
                ShaderVariable(nbdstr(), groupid[0] * threadDim[0] + tx,
                               groupid[1] * threadDim[1] + ty, groupid[2] * threadDim[2] + tz, 0U);
            thread_builtins[ShaderBuiltin::GroupThreadIndex] =
                ShaderVariable(nbdstr(), tx, ty, tz, 0U);
            thread_builtins[ShaderBuiltin::GroupFlatIndex] = ShaderVariable(
                nbdstr(), tz * threadDim[0] * threadDim[1] + ty * threadDim[0] + tx, 0U, 0U, 0U);
            apiWrapper->thread_props[i][(size_t)nbdspv::ThreadProperty::Active] = 1;

            if(hasQuadScope)
            {
              uint32_t quadX = (tx / quadW);
              uint32_t quadY = (ty / quadH);
              uint32_t quadZ = tz;
              uint32_t quadId =
                  quadIdOffset + quadX + (quadY * countQuadX) + (quadZ * countQuadY * countQuadX);
              uint32_t quadLaneIndex = (tx % quadW) + (ty % quadH) * 2;

              apiWrapper->thread_props[i][(size_t)nbdspv::ThreadProperty::QuadLane] = quadLaneIndex;
              apiWrapper->thread_props[i][(size_t)nbdspv::ThreadProperty::QuadId] = quadId;
            }

            if(nbdfixedarray<uint32_t, 3>({tx, ty, tz}) == threadid)
            {
              laneIndex = i;
            }

            i++;
          }
        }
      }
    }
    else if(hasQuadScope)
    {
      // need to simulate the whole quad, do not readback from the GPU like we do with subgroups
      // the quad is guaranteed to be in the same subgroup
      // We lay things out in linear or quad order
      NBDASSERTEQUAL(numThreads, 4U);
      uint32_t txMin = (threadid[0] / quadW) * quadW;
      uint32_t tyMin = (threadid[1] / quadH) * quadH;
      uint32_t tz = threadid[2];
      uint32_t quadZ = tz;
      for(uint32_t i = 0; i < 4U; ++i)
      {
        uint32_t tx = txMin + (i % quadW);
        uint32_t ty = tyMin + (i / quadW);
        std::unordered_map<ShaderBuiltin, ShaderVariable> &thread_builtins = allthread_builtins[i];
        thread_builtins[ShaderBuiltin::DispatchThreadIndex] =
            ShaderVariable(nbdstr(), groupid[0] * threadDim[0] + tx, groupid[1] * threadDim[1] + ty,
                           groupid[2] * threadDim[2] + tz, 0U);
        thread_builtins[ShaderBuiltin::GroupThreadIndex] = ShaderVariable(nbdstr(), tx, ty, tz, 0U);
        thread_builtins[ShaderBuiltin::GroupFlatIndex] = ShaderVariable(
            nbdstr(), tz * threadDim[0] * threadDim[1] + ty * threadDim[0] + tx, 0U, 0U, 0U);
        apiWrapper->thread_props[i][(size_t)nbdspv::ThreadProperty::Active] = 1;

        uint32_t quadX = (tx / quadW);
        uint32_t quadY = (ty / quadH);
        uint32_t quadId =
            quadIdOffset + quadX + (quadY * countQuadX) + (quadZ * countQuadY * countQuadX);
        uint32_t quadLaneIndex = (tx % quadW) + (ty % quadH) * 2;

        apiWrapper->thread_props[i][(size_t)nbdspv::ThreadProperty::QuadLane] = quadLaneIndex;
        apiWrapper->thread_props[i][(size_t)nbdspv::ThreadProperty::QuadId] = quadId;

        if(nbdfixedarray<uint32_t, 3>({tx, ty, tz}) == threadid)
          laneIndex = i;
      }
    }
    else
    {
      NBDASSERTEQUAL(numThreads, 1U);
      // simple single-thread case
      apiWrapper->thread_props[0][(size_t)nbdspv::ThreadProperty::Active] = 1;
      apiWrapper->thread_props[0][(size_t)nbdspv::ThreadProperty::SubgroupId] = 0;

      std::unordered_map<ShaderBuiltin, ShaderVariable> &thread_builtins = allthread_builtins[0];

      thread_builtins[ShaderBuiltin::DispatchThreadIndex] = ShaderVariable(
          nbdstr(), groupid[0] * threadDim[0] + threadid[0],
          groupid[1] * threadDim[1] + threadid[1], groupid[2] * threadDim[2] + threadid[2], 0U);
      thread_builtins[ShaderBuiltin::GroupThreadIndex] =
          ShaderVariable(nbdstr(), threadid[0], threadid[1], threadid[2], 0U);
      thread_builtins[ShaderBuiltin::GroupFlatIndex] = ShaderVariable(
          nbdstr(),
          threadid[2] * threadDim[0] * threadDim[1] + threadid[1] * threadDim[0] + threadid[0], 0U,
          0U, 0U);
    }

    nbdspv::Debugger *debugger = new nbdspv::Debugger;
    debugger->Parse(shader.spirv.GetSPIRV());

    global_builtins[ShaderBuiltin::SubgroupSize] = ShaderVariable(nbdstr(), 1U, 0U, 0U, 0U);

    apiWrapper->SetInputVarsToReadOnly();
    ShaderDebugTrace *ret =
        debugger->BeginDebug(apiWrapper, stage, entryPoint, spec, shadRefl.instructionLines,
                             shadRefl.patchData, laneIndex, numThreads, 1);
    apiWrapper->ResetReplay();

    return ret;
  }
}

nbdarray<ShaderDebugState> VulkanReplay::ContinueDebug(ShaderDebugger *debugger)
{
  nbdspv::Debugger *spvDebugger = (nbdspv::Debugger *)debugger;

  if(!spvDebugger)
    return {};

  VkMarkerRegion region("ContinueDebug Simulation Loop");

  for(size_t fmt = 0; fmt < ARRAY_COUNT(m_TexRender.DummyImageViews); fmt++)
  {
    for(size_t dim = 0; dim < ARRAY_COUNT(m_TexRender.DummyImageViews[0]); dim++)
    {
      if(m_TexRender.DummyImageViews[fmt][dim] == VK_NULL_HANDLE)
        continue;

      m_ShaderDebugData.DummyImageInfos[fmt][dim].imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      m_ShaderDebugData.DummyImageInfos[fmt][dim].imageView =
          Unwrap(m_TexRender.DummyImageViews[fmt][dim]);

      m_ShaderDebugData.DummyWrites[fmt][dim].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      m_ShaderDebugData.DummyWrites[fmt][dim].descriptorCount = 1;
      m_ShaderDebugData.DummyWrites[fmt][dim].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      m_ShaderDebugData.DummyWrites[fmt][dim].dstBinding = uint32_t(dim + 1);
      m_ShaderDebugData.DummyWrites[fmt][dim].dstSet = VK_NULL_HANDLE;
      m_ShaderDebugData.DummyWrites[fmt][dim].pImageInfo =
          &m_ShaderDebugData.DummyImageInfos[fmt][dim];
    }

    m_ShaderDebugData.DummyImageInfos[fmt][5].sampler = Unwrap(m_TexRender.DummySampler);

    m_ShaderDebugData.DummyWrites[fmt][5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    m_ShaderDebugData.DummyWrites[fmt][5].descriptorCount = 1;
    m_ShaderDebugData.DummyWrites[fmt][5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    m_ShaderDebugData.DummyWrites[fmt][5].dstBinding = (uint32_t)ShaderDebugBind::Sampler;
    m_ShaderDebugData.DummyWrites[fmt][5].dstSet = VK_NULL_HANDLE;
    m_ShaderDebugData.DummyWrites[fmt][5].pImageInfo = &m_ShaderDebugData.DummyImageInfos[fmt][5];

    if(m_TexRender.DummyBufferView[fmt] != VK_NULL_HANDLE)
    {
      m_ShaderDebugData.DummyWrites[fmt][6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      m_ShaderDebugData.DummyWrites[fmt][6].descriptorCount = 1;
      m_ShaderDebugData.DummyWrites[fmt][6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
      m_ShaderDebugData.DummyWrites[fmt][6].dstBinding = (uint32_t)ShaderDebugBind::Buffer;
      m_ShaderDebugData.DummyWrites[fmt][6].dstSet = VK_NULL_HANDLE;
      m_ShaderDebugData.DummyWrites[fmt][6].pTexelBufferView =
          UnwrapPtr(m_TexRender.DummyBufferView[fmt]);
    }
  }

  nbdarray<ShaderDebugState> ret = spvDebugger->ContinueDebug();

  VulkanAPIWrapper *api = (VulkanAPIWrapper *)spvDebugger->GetAPIWrapper();
  api->ResetReplay();

  return ret;
}

void VulkanReplay::FreeDebugger(ShaderDebugger *debugger)
{
  delete debugger;
  Threading::JobSystem::SyncAllJobs();
}
