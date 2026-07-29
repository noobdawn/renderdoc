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

#include <ctype.h>
#include <float.h>
#include "common/formatting.h"
#include "core/settings.h"
#include "driver/shaders/spirv/spirv_editor.h"
#include "driver/shaders/spirv/spirv_op_helpers.h"
#include "vk_core.h"
#include "vk_debug.h"
#include "vk_replay.h"
#include "vk_shader_cache.h"

RDOC_CONFIG(nbdstr, Vulkan_Debug_FeedbackDumpDirPath, "",
            "Path to dump bindless feedback annotation generated SPIR-V files.");
RDOC_CONFIG(
    bool, Vulkan_BindlessFeedback, true,
    "Enable fetching from GPU which descriptors were dynamically used in descriptor arrays.");
RDOC_CONFIG(bool, Vulkan_PrintfFetch, true, "Enable fetching printf messages from GPU.");
RDOC_CONFIG(uint32_t, Vulkan_Debug_PrintfBufferSize, 64 * 1024,
            "How many bytes to reserve for a printf output buffer.");

static const uint32_t ShaderStageHeaderBitShift = 28U;
static const uint32_t ShaderFeedbackReservedBindings = 1;

struct BindKey
{
  bool operator<(const BindKey &o) const
  {
    if(stage != o.stage)
      return stage < o.stage;
    return index < o.index;
  }

  bool operator!=(const BindKey &o) const { return !operator==(o); }
  bool operator==(const BindKey &o) const { return stage == o.stage && index == o.index; }

  ShaderStage stage;
  ShaderBindIndex index;
};

struct BindData
{
  uint64_t offset;
  uint32_t numEntries;

  DescriptorAccess access;
};

struct BindlessFeedbackData
{
  std::map<BindKey, BindData> offsetMap;
  uint32_t feedbackStorageSize = 0;
};

struct PrintfData
{
  nbdstr user_format;
  nbdstr effective_format;
  // vectors are expanded so there's one for each component (as printf will expect)
  nbdarray<nbdspv::Scalar> argTypes;
  size_t payloadWords;
};

struct ShaderPrintfArgs : public StringFormat::Args
{
public:
  ShaderPrintfArgs(const uint32_t *payload, const PrintfData &formats)
      : m_Start(payload), m_Cur(payload), m_Idx(0), m_Formats(formats)
  {
  }

  void reset() override
  {
    m_Cur = m_Start;
    m_Idx = 0;
  }
  void error(const char *err) override { m_Error = err; }
  int get_int() override
  {
    int32_t ret = *(int32_t *)m_Cur;
    m_Idx++;
    m_Cur++;
    return ret;
  }
  unsigned int get_uint() override
  {
    uint32_t ret = *(uint32_t *)m_Cur;
    m_Idx++;
    m_Cur++;
    return ret;
  }
  double get_double() override
  {
    // here we need to know if a real double was stored or not. It probably isn't but we handle it
    if(m_Idx < m_Formats.argTypes.size())
    {
      if(m_Formats.argTypes[m_Idx].width == 64)
      {
        double ret = *(double *)m_Cur;
        m_Idx++;
        m_Cur += 2;
        return ret;
      }
      else
      {
        float ret = *(float *)m_Cur;
        m_Idx++;
        m_Cur++;
        return ret;
      }
    }
    else
    {
      return 0.0;
    }
  }
  void *get_ptr() override
  {
    m_Idx++;
    return NULL;
  }
  const char *get_str() override
  {
    m_Idx++;
    return NULL;
  }
  uint64_t get_uint64() override
  {
    uint64_t ret = *(uint64_t *)m_Cur;
    m_Idx++;
    m_Cur += 2;
    return ret;
  }

  nbdstr get_error() { return m_Error; }
private:
  const uint32_t *m_Cur;
  const uint32_t *m_Start;
  size_t m_Idx;
  const PrintfData &m_Formats;
  nbdstr m_Error;
};

nbdstr PatchFormatString(nbdstr format)
{
  // we don't support things like %XX.YYv2f so look for vector formatters and expand them to
  // %XX.YYf, %XX.YYf
  // Also annoyingly the printf specification for 64-bit integers is printed as %ul instead of %llu,
  // so we need to patch that up too

  for(size_t i = 0; i < format.size(); i++)
  {
    if(format[i] == '%')
    {
      size_t start = i;

      i++;
      if(format[i] == '%')
        continue;

      // skip to first letter
      while(i < format.size() && !isalpha(format[i]))
        i++;

      // malformed string, abort
      if(!isalpha(format[i]))
      {
        NBDERR("Malformed format string '%s'", format.c_str());
        break;
      }

      // if the first letter is v, this is a vector format
      if(format[i] == 'v' || format[i] == 'V')
      {
        size_t vecStart = i;

        int vecsize = int(format[i + 1]) - int('0');

        if(vecsize < 2 || vecsize > 4)
        {
          NBDERR("Malformed format string '%s'", format.c_str());
          break;
        }

        // skip the v and the [234]
        i += 2;

        if(i >= format.size())
        {
          NBDERR("Malformed format string '%s'", format.c_str());
          break;
        }

        bool int64 = false;
        // if the final letter is u, we need to peek ahead to see if there's a l following
        if(format[i] == 'u' && i + 1 < format.size() && format[i + 1] == 'l')
        {
          i++;
          int64 = true;
        }

        nbdstr componentFormat = format.substr(start, i - start + 1);

        // remove the vX from the component format
        componentFormat.erase(vecStart - start, 2);

        // if it's a 64-bit ul, transform to llu
        if(int64)
        {
          componentFormat.pop_back();
          componentFormat.pop_back();
          componentFormat += "llu";
        }

        nbdstr vectorExpandedFormat;
        for(int v = 0; v < vecsize; v++)
        {
          vectorExpandedFormat += componentFormat;
          if(v + 1 < vecsize)
            vectorExpandedFormat += ", ";
        }

        // remove the vector formatter
        format.erase(start, i - start + 1);
        format.insert(start, vectorExpandedFormat);

        continue;
      }

      // if the letter is u, see if the next is l. If so we translate ul to llu
      if(format[i] == 'u' && i + 1 < format.size() && format[i + 1] == 'l')
      {
        format[i] = 'l';
        format[i + 1] = 'u';
        format.insert(i, 'l');
      }
    }
  }

  return format;
}

// uintvulkanmax_t is uint64_t if int64 is supported in shaders, otherwise uint32_t
// if it's int32 we just truncate all maths and assume things won't overflow
template <typename uintvulkanmax_t>
nbdspv::Id MakeOffsettedPointer(nbdspv::Editor &editor, nbdspv::Iter &it, nbdspv::Id ptrType,
                                nbdspv::Id carryStructType, nbdspv::Id bufferAddressConst,
                                nbdspv::Id offset);

// easy case with uint64, we do an IAdd then a ConvertUToPtr
template <>
nbdspv::Id MakeOffsettedPointer<uint64_t>(nbdspv::Editor &editor, nbdspv::Iter &it,
                                          nbdspv::Id ptrType, nbdspv::Id carryStructType,
                                          nbdspv::Id bufferAddressConst, nbdspv::Id offset)
{
  if(offset == nbdspv::Id())
    return editor.AddOperation(it, nbdspv::OpBitcast(ptrType, editor.MakeId(), bufferAddressConst));

  nbdspv::Id uint64Type = editor.DeclareType(nbdspv::scalar<uint64_t>());

  // first bitcast to uint64 for addition
  nbdspv::Id base =
      editor.AddOperation(it, nbdspv::OpBitcast(uint64Type, editor.MakeId(), bufferAddressConst));
  it++;

  // add the offset
  nbdspv::Id finalAddr =
      editor.AddOperation(it, nbdspv::OpIAdd(uint64Type, editor.MakeId(), base, offset));
  it++;

  // convert to pointer
  return editor.AddOperation(it, nbdspv::OpConvertUToPtr(ptrType, editor.MakeId(), finalAddr));
}

// hard case with {uint32,uint32}
template <>
nbdspv::Id MakeOffsettedPointer<uint32_t>(nbdspv::Editor &editor, nbdspv::Iter &it,
                                          nbdspv::Id ptrType, nbdspv::Id carryStructType,
                                          nbdspv::Id bufferAddressConst, nbdspv::Id offset)
{
  nbdspv::Id finalAddr = bufferAddressConst;

  if(offset != nbdspv::Id())
  {
    nbdspv::Id uint32Type = editor.DeclareType(nbdspv::scalar<uint32_t>());
    nbdspv::Id uintVec = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<uint32_t>(), 2));

    // pull the lsb/msb out of the vector
    nbdspv::Id lsb = editor.AddOperation(
        it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), bufferAddressConst, {0}));
    it++;
    nbdspv::Id msb = editor.AddOperation(
        it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), bufferAddressConst, {1}));
    it++;

    // add the offset to the LSB and allow it to carry
    nbdspv::Id offsetWithCarry =
        editor.AddOperation(it, nbdspv::OpIAddCarry(carryStructType, editor.MakeId(), lsb, offset));
    it++;

    // extract the result to the new lsb, and carry
    lsb = editor.AddOperation(
        it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), offsetWithCarry, {0}));
    it++;
    nbdspv::Id carry = editor.AddOperation(
        it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), offsetWithCarry, {1}));
    it++;

    // add carry bit to msb
    msb = editor.AddOperation(it, nbdspv::OpIAdd(uint32Type, editor.MakeId(), msb, carry));
    it++;

    // construct a vector again
    finalAddr =
        editor.AddOperation(it, nbdspv::OpCompositeConstruct(uintVec, editor.MakeId(), {lsb, msb}));
    it++;
  }

  // bitcast the vector to a pointer
  return editor.AddOperation(it, nbdspv::OpBitcast(ptrType, editor.MakeId(), finalAddr));
}

template <typename uintvulkanmax_t>
void AnnotateShader(const ShaderReflection &refl, const SPIRVPatchData &patchData, ShaderStage stage,
                    const char *entryName, const std::map<BindKey, BindData> &offsetMap,
                    uint32_t maxSlot, bool usePrimitiveID, VkDeviceAddress addr,
                    BufferStorageMode storageMode, bool usesMultiview, nbdarray<uint32_t> &modSpirv,
                    std::map<uint32_t, PrintfData> &printfData)
{
  // calculate offsets for IDs on the original unmodified SPIR-V. The editor may insert some nops,
  // so we do it manually here
  std::map<nbdspv::Id, uint32_t> idToOffset;

  for(nbdspv::Iter it(modSpirv, nbdspv::FirstRealWord); it; it++)
    idToOffset[nbdspv::OpDecoder(it).result] = (uint32_t)it.offs();

  nbdspv::Editor editor(modSpirv);

  editor.Prepare();
  editor.SetBufferStorageMode(storageMode);

  editor.OffsetBindingsToMatchReservation(ShaderFeedbackReservedBindings);

  NBDASSERTMSG("SPIR-V module is too large to encode instruction ID!", modSpirv.size() < 0xfffffffU);

  const uint32_t targetIndexWidth = IsBDA(storageMode) ? sizeof(uintvulkanmax_t) * 8 : 32;

  // store the maximum slot we can use, for clamping outputs to avoid writing out of bounds
  nbdspv::Id maxSlotID = targetIndexWidth == 64 ? editor.AddConstantImmediate<uint64_t>(maxSlot)
                                                : editor.AddConstantImmediate<uint32_t>(maxSlot);
  nbdspv::Id maxSlotByteOffsetID =
      targetIndexWidth == 64 ? editor.AddConstantImmediate<uint64_t>(maxSlot * sizeof(uint32_t))
                             : editor.AddConstantImmediate<uint32_t>(maxSlot * sizeof(uint32_t));

  nbdspv::Id maxPrintfWordOffset =
      editor.AddConstantImmediate<uint32_t>(Vulkan_Debug_PrintfBufferSize() / sizeof(uint32_t));

  nbdspv::Id falsePrintfValue = editor.AddConstantImmediate<uint32_t>(0U);
  nbdspv::Id truePrintfValue = editor.AddConstantImmediate<uint32_t>(1U);

  nbdspv::Id uint32Type = editor.DeclareType(nbdspv::scalar<uint32_t>());
  nbdspv::Id int32Type = editor.DeclareType(nbdspv::scalar<int32_t>());
  nbdspv::Id f32Type = editor.DeclareType(nbdspv::scalar<float>());
  nbdspv::Id uint64Type;
  nbdspv::Id outputBufVarType;
  nbdspv::Id indexOffsetType;

  // if the module declares int64 capability, or we use it, ensure uint64 is declared in case we
  // need to transform it for printf arguments
  if(editor.HasCapability(nbdspv::Capability::Int64) || targetIndexWidth == 64)
  {
    editor.AddCapability(nbdspv::Capability::Int64);
    uint64Type = editor.DeclareType(nbdspv::scalar<uint64_t>());
  }

  // for BDA we do indexing math on the pointer then convert so we don't need a container struct,
  // for descriptor-based access we need to declare our struct up front with a runtime array of ints
  if(IsBDA(storageMode))
  {
    // any function parameters we add are byte offsets
    indexOffsetType = editor.DeclareType(nbdspv::scalar<uintvulkanmax_t>());

    outputBufVarType = uint32Type;
  }
  else
  {
    nbdspv::Id runtimeArrayID =
        editor.AddType(nbdspv::OpTypeRuntimeArray(editor.MakeId(), uint32Type));

    editor.AddDecoration(nbdspv::OpDecorate(
        runtimeArrayID, nbdspv::DecorationParam<nbdspv::Decoration::ArrayStride>(sizeof(uint32_t))));

    nbdspv::Id uint32StructID =
        editor.AddType(nbdspv::OpTypeStruct(editor.MakeId(), {runtimeArrayID}));

    editor.SetName(uint32StructID, "__rd_feedbackStruct");

    editor.AddDecoration(nbdspv::OpMemberDecorate(
        uint32StructID, 0, nbdspv::DecorationParam<nbdspv::Decoration::Offset>(0)));

    // any function parameters we add are uint32 indices
    indexOffsetType = uint32Type;

    outputBufVarType = uint32StructID;
  }

  // map from variable ID to watch, to variable ID to get offset from (as a SPIR-V constant,
  // or as either byte offset for buffer addressing or ssbo index otherwise)
  std::map<nbdspv::Id, nbdspv::Id> varLookup;

  // iterate over all variables. We do this here because in the absence of the buffer address
  // extension we might declare our own below and patch bindings - so we need to look these up now
  for(const nbdspv::Variable &var : editor.GetGlobals())
  {
    // skip variables without one of these storage classes, as they are not descriptors
    if(var.storage != nbdspv::StorageClass::UniformConstant &&
       var.storage != nbdspv::StorageClass::Uniform &&
       var.storage != nbdspv::StorageClass::StorageBuffer)
      continue;

    // figure out which interface this variable is in to make our key
    BindKey key = {};
    key.stage = refl.stage;

    int32_t idx = -1;
    if((idx = patchData.cblockInterface.indexOf(var.id)) >= 0)
    {
      key.index.category = DescriptorCategory::ConstantBlock;
      key.index.index = (uint32_t)idx;
    }
    else if((idx = patchData.samplerInterface.indexOf(var.id)) >= 0)
    {
      key.index.category = DescriptorCategory::Sampler;
      key.index.index = (uint32_t)idx;
    }
    else if((idx = patchData.roInterface.indexOf(var.id)) >= 0)
    {
      key.index.category = DescriptorCategory::ReadOnlyResource;
      key.index.index = (uint32_t)idx;
    }
    else if((idx = patchData.rwInterface.indexOf(var.id)) >= 0)
    {
      key.index.category = DescriptorCategory::ReadWriteResource;
      key.index.index = (uint32_t)idx;
    }

    // if this is one of the bindings we care about
    auto it = offsetMap.find(key);
    if(it != offsetMap.end())
    {
      // store the offset for this variable so we watch for access chains and know where to store to
      if(IsBDA(storageMode))
      {
        nbdspv::Id id = varLookup[var.id] =
            editor.AddConstantImmediate<uintvulkanmax_t>(uintvulkanmax_t(it->second.offset));

        editor.SetName(
            id, StringFormat::Fmt("__feedbackOffset_%s_%u", ToStr(it->first.index.category).c_str(),
                                  it->first.index.index));
      }
      else
      {
        // check that the offset fits in 32-bit word, convert byte offset to uint32 index
        uint64_t index = it->second.offset / 4;
        NBDASSERT(index < 0xFFFFFFFFULL, it->first.index.category, it->first.index.index,
                  it->second.offset);
        nbdspv::Id id = varLookup[var.id] = editor.AddConstantImmediate<uint32_t>(uint32_t(index));

        editor.SetName(
            id, StringFormat::Fmt("__feedbackOffset_%s_%u", ToStr(it->first.index.category).c_str(),
                                  it->first.index.index));
      }
    }
  }

  nbdspv::Id carryStructType = editor.DeclareStructType({uint32Type, uint32Type});

  if(usesMultiview &&
     (stage == ShaderStage::Pixel || stage == ShaderStage::Vertex || stage == ShaderStage::Geometry))
  {
    editor.AddCapability(nbdspv::Capability::MultiView);
    editor.AddExtension("SPV_KHR_multiview");
  }

  if(usePrimitiveID && stage == ShaderStage::Fragment && Vulkan_PrintfFetch())
  {
    editor.AddCapability(nbdspv::Capability::Geometry);
  }

  nbdarray<nbdspv::Id> newGlobals;
  nbdspv::StorageClass bufferClass = editor.PrepareAddedBufferAccess();

  nbdpair<nbdspv::Id, nbdspv::Id> feedbackBuffer =
      editor.AddBufferVariable(newGlobals, outputBufVarType, "__rd_feedbackBuffer", 0, 0, addr);

  nbdspv::Id uint32ptrtype = editor.DeclareType(nbdspv::Pointer(uint32Type, bufferClass));

  nbdspv::Id rtarrayOffset = editor.AddConstantImmediate<uint32_t>(0U);
  nbdspv::Id printfArrayOffset = rtarrayOffset;
  nbdspv::Id zero = rtarrayOffset;
  nbdspv::Id usedValue = editor.AddConstantImmediate<uint32_t>(0xFFFFFFFFU);
  nbdspv::Id scope = editor.AddConstantImmediate<uint32_t>((uint32_t)nbdspv::Scope::Invocation);
  nbdspv::Id semantics = editor.AddConstantImmediate<uint32_t>(0U);
  nbdspv::Id uint32shift = editor.AddConstantImmediate<uint32_t>(2U);

  nbdspv::MemoryAccessAndParamDatas memoryAccess;
  memoryAccess.setAligned(sizeof(uint32_t));

  nbdspv::Id printfIncrement;

  if(IsBDA(storageMode))
  {
    printfIncrement = editor.AddConstantImmediate<uintvulkanmax_t>(sizeof(uint32_t));
  }
  else
  {
    printfIncrement = editor.AddConstantImmediate<uint32_t>(1U);
  }

  nbdspv::Id glsl450 = editor.ImportExtInst("GLSL.std.450");

  std::map<nbdspv::Id, nbdspv::Scalar> intTypeLookup;

  for(auto scalarType : editor.GetTypeInfo<nbdspv::Scalar>())
    if(scalarType.first.type == nbdspv::Op::TypeInt)
      intTypeLookup[scalarType.second] = scalarType.first;

  nbdspv::Id entryID;
  for(const nbdspv::EntryPoint &entry : editor.GetEntries())
  {
    if(entry.name == entryName && MakeShaderStage(entry.executionModel) == stage)
    {
      entryID = entry.id;
      break;
    }
  }

  nbdspv::Id uvec2Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<uint32_t>(), 2));
  nbdspv::Id uvec3Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<uint32_t>(), 3));
  nbdspv::Id uvec4Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<uint32_t>(), 4));

  // we'll initialise this at the start of the entry point, and use it globally to get the location
  // for printf statements
  nbdspv::Id printfLocationVar = editor.MakeId();

  if(Vulkan_PrintfFetch())
  {
    editor.AddVariable(nbdspv::OpVariable(
        editor.DeclareType(nbdspv::Pointer(uvec4Type, nbdspv::StorageClass::Private)),
        printfLocationVar, nbdspv::StorageClass::Private));

    if(editor.EntryPointAllGlobals())
      newGlobals.push_back(printfLocationVar);
  }

  nbdspv::Id shaderStageConstant =
      editor.AddConstantImmediate<uint32_t>(uint32_t(stage) << ShaderStageHeaderBitShift);
  nbdspv::Id int64wordshift = editor.AddConstantImmediate<uint32_t>(32U);

  // build up operations to pull in the location from globals - either existing or ones we add
  nbdspv::OperationList locationGather;

  if(Vulkan_PrintfFetch())
  {
    nbdarray<nbdspv::Id> idxs;

    auto fetchOrAddGlobalInput = [&editor, &idxs, &refl, &patchData, &locationGather, &newGlobals](
                                     const char *name, ShaderBuiltin builtin,
                                     nbdspv::BuiltIn spvBuiltin, nbdspv::Id varType, bool integer) {
      nbdspv::Id ret;

      nbdspv::Id ptrType = editor.DeclareType(nbdspv::Pointer(varType, nbdspv::StorageClass::Input));

      for(size_t i = 0; i < refl.inputSignature.size(); i++)
      {
        if(refl.inputSignature[i].systemValue == builtin)
        {
          nbdspv::Id loadType = varType;
          if(refl.inputSignature[i].varType == VarType::SInt)
          {
            if(refl.inputSignature[i].compCount == 1)
              loadType = editor.DeclareType(nbdspv::scalar<int32_t>());
            else
              loadType = editor.DeclareType(
                  nbdspv::Vector(nbdspv::scalar<int32_t>(), refl.inputSignature[i].compCount));
          }

          if(patchData.inputs[i].accessChain.empty())
          {
            ret =
                locationGather.add(nbdspv::OpLoad(loadType, editor.MakeId(), patchData.inputs[i].ID));
          }
          else
          {
            nbdarray<nbdspv::Id> chain;

            for(uint32_t accessIdx : patchData.inputs[i].accessChain)
            {
              idxs.resize_for_index(accessIdx);
              if(idxs[accessIdx] == 0)
                idxs[accessIdx] = editor.AddConstantImmediate<uint32_t>(accessIdx);

              chain.push_back(idxs[accessIdx]);
            }

            nbdspv::Id subElement = locationGather.add(
                nbdspv::OpAccessChain(ptrType, editor.MakeId(), patchData.inputs[i].ID, chain));

            ret = locationGather.add(nbdspv::OpLoad(loadType, editor.MakeId(), subElement));
          }

          if(loadType != varType)
            ret = locationGather.add(nbdspv::OpBitcast(varType, editor.MakeId(), ret));
        }
      }

      if(ret == nbdspv::Id())
      {
        nbdspv::Id rdocGlobalVar = editor.AddVariable(
            nbdspv::OpVariable(ptrType, editor.MakeId(), nbdspv::StorageClass::Input));
        editor.AddDecoration(nbdspv::OpDecorate(
            rdocGlobalVar, nbdspv::DecorationParam<nbdspv::Decoration::BuiltIn>(spvBuiltin)));
        // Fragment shader inputs that are signed or unsigned integers, integer vectors, or any
        // double-precision floating-point type must be decorated with Flat.
        if(integer && refl.stage == ShaderStage::Pixel)
          editor.AddDecoration(nbdspv::OpDecorate(rdocGlobalVar, nbdspv::Decoration::Flat));

        newGlobals.push_back(rdocGlobalVar);

        editor.SetName(rdocGlobalVar, name);

        ret = locationGather.add(nbdspv::OpLoad(varType, editor.MakeId(), rdocGlobalVar));
      }

      return ret;
    };

    nbdspv::Id location;

    // the location encoding varies by stage
    if(stage == ShaderStage::Compute)
    {
      // the location for compute is easy, it's just the global invocation
      location = fetchOrAddGlobalInput("rdoc_invocation", ShaderBuiltin::DispatchThreadIndex,
                                       nbdspv::BuiltIn::GlobalInvocationId, uvec3Type, true);

      location = locationGather.add(
          nbdspv::OpVectorShuffle(uvec4Type, editor.MakeId(), location, location, {0, 1, 2, 0}));
    }
    else if(stage == ShaderStage::Task)
    {
      // the location for task shaders is the same
      location = fetchOrAddGlobalInput("rdoc_invocation", ShaderBuiltin::DispatchThreadIndex,
                                       nbdspv::BuiltIn::GlobalInvocationId, uvec3Type, true);

      location = locationGather.add(
          nbdspv::OpVectorShuffle(uvec4Type, editor.MakeId(), location, location, {0, 1, 2, 0}));
    }
    else if(stage == ShaderStage::Mesh)
    {
      // the location for mesh shaders is packed a smidge tighter.
      // we need three 3D locators:
      //   (optional) task group index
      //   mesh group index
      //   local thread index
      //
      // the local index has a compile-time known stride so we can use the linear index, which we
      // can give 16 bits to be very generous (10 bits is a more realistic upper bound)
      //
      // similarly the task group index has a known stride so we can use a linear index for it as
      // well. Giving it 32 bits covers any reasonable use (~26 bits is the max reported at the time
      // of writing)
      //
      // annoyingly this leaves us 48 bits per task group index dimension. That is enough for a
      // linear ID easily but it does not have a easily known stride (for a task shader it depends
      // on the OpEmitMeshTasksEXT dimensions). It's not enough for the worst case in each dimension
      // which some drivers report as [4194304,65535,65535] which requires 22,16,16 bits. Those
      // drivers don't allow a shader to dispatch that many in all dimensions as the product is
      // still constrained.
      //
      // So instead we've just used 4 uints for the location just for the mesh shader. We still have
      // to compress things a little so we put the mesh thread in the upper 16-bits with mesh group
      // z
      nbdspv::Id meshThread =
          fetchOrAddGlobalInput("rdoc_meshThread", ShaderBuiltin::GroupFlatIndex,
                                nbdspv::BuiltIn::LocalInvocationIndex, uint32Type, true);
      nbdspv::Id meshGroup = fetchOrAddGlobalInput("rdoc_meshGroup", ShaderBuiltin::GroupIndex,
                                                   nbdspv::BuiltIn::WorkgroupId, uvec3Type, true);

      // TODO read task ID from payload
      nbdspv::Id taskId = zero;

      nbdspv::Id meshThreadShifted = locationGather.add(nbdspv::OpShiftLeftLogical(
          uint32Type, editor.MakeId(), meshThread, editor.AddConstantImmediate<uint32_t>(16U)));
      nbdspv::Id meshGroupX =
          locationGather.add(nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), meshGroup, {0}));
      nbdspv::Id meshGroupY =
          locationGather.add(nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), meshGroup, {1}));
      nbdspv::Id meshGroupZ =
          locationGather.add(nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), meshGroup, {2}));
      meshGroupZ = locationGather.add(nbdspv::OpBitwiseAnd(
          uint32Type, editor.MakeId(), meshGroupZ, editor.AddConstantImmediate<uint32_t>(0xffff)));
      meshGroupZ = locationGather.add(
          nbdspv::OpBitwiseOr(uint32Type, editor.MakeId(), meshGroupZ, meshThreadShifted));

      location = locationGather.add(nbdspv::OpCompositeConstruct(
          uvec4Type, editor.MakeId(), {meshGroupX, meshGroupY, meshGroupZ, taskId}));
    }
    else if(stage == ShaderStage::Vertex || stage == ShaderStage::Pixel)
    {
      nbdspv::Id view;

      // only search for the view index is the multiview capability is declared, otherwise it's
      // invalid and we just set 0. Valid for both Vertex and Pixel shaders
      if(editor.HasCapability(nbdspv::Capability::MultiView))
      {
        view = fetchOrAddGlobalInput("rdoc_viewIndex", ShaderBuiltin::MultiViewIndex,
                                     nbdspv::BuiltIn::ViewIndex, uint32Type, true);
      }
      else
      {
        view = editor.AddConstantImmediate<uint32_t>(0U);
      }

      if(stage == ShaderStage::Vertex)
      {
        nbdspv::Id vtx = fetchOrAddGlobalInput("rdoc_vertexIndex", ShaderBuiltin::VertexIndex,
                                               nbdspv::BuiltIn::VertexIndex, uint32Type, true);
        nbdspv::Id inst = fetchOrAddGlobalInput("rdoc_instanceIndex", ShaderBuiltin::InstanceIndex,
                                                nbdspv::BuiltIn::InstanceIndex, uint32Type, true);

        location = locationGather.add(
            nbdspv::OpCompositeConstruct(uvec4Type, editor.MakeId(), {vtx, inst, view, zero}));
      }
      else if(stage == ShaderStage::Pixel)
      {
        nbdspv::Id float2Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), 2));
        nbdspv::Id float4Type = editor.DeclareType(nbdspv::Vector(nbdspv::scalar<float>(), 4));

        nbdspv::Id coord = fetchOrAddGlobalInput("rdoc_fragCoord", ShaderBuiltin::Position,
                                                 nbdspv::BuiltIn::FragCoord, float4Type, false);

        // grab just the xy
        coord = locationGather.add(
            nbdspv::OpVectorShuffle(float2Type, editor.MakeId(), coord, coord, {0, 1}));

        // convert to int
        coord = locationGather.add(nbdspv::OpConvertFToU(uvec2Type, editor.MakeId(), coord));

        nbdspv::Id x =
            locationGather.add(nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), coord, {0}));
        nbdspv::Id y =
            locationGather.add(nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), coord, {1}));

        // shift x up into top 16-bits
        x = locationGather.add(nbdspv::OpShiftLeftLogical(
            uint32Type, editor.MakeId(), x, editor.AddConstantImmediate<uint32_t>(16U)));

        // OR together
        coord = locationGather.add(nbdspv::OpBitwiseOr(uint32Type, editor.MakeId(), x, y));

        nbdspv::Id samp;

        // only grab the sample ID if sample shading is already enabled
        for(size_t i = 0; i < refl.inputSignature.size(); i++)
        {
          if(refl.inputSignature[i].systemValue == ShaderBuiltin::MSAASampleIndex ||
             refl.inputSignature[i].systemValue == ShaderBuiltin::MSAASamplePosition)
          {
            samp = fetchOrAddGlobalInput("rdoc_sampleIndex", ShaderBuiltin::MSAASampleIndex,
                                         nbdspv::BuiltIn::SampleId, uint32Type, true);
          }
        }

        if(samp == nbdspv::Id())
        {
          samp = editor.AddConstantImmediate<uint32_t>(~0U);
        }

        // shift samp up into top 16-bits
        samp = locationGather.add(nbdspv::OpShiftLeftLogical(
            uint32Type, editor.MakeId(), samp, editor.AddConstantImmediate<uint32_t>(16U)));

        // OR samp and view together
        view = locationGather.add(nbdspv::OpBitwiseOr(uint32Type, editor.MakeId(), samp, view));

        nbdspv::Id prim;

        if(usePrimitiveID)
        {
          prim = fetchOrAddGlobalInput("rdoc_primitiveIndex", ShaderBuiltin::PrimitiveIndex,
                                       nbdspv::BuiltIn::PrimitiveId, uint32Type, true);
        }
        else
        {
          prim = editor.AddConstantImmediate<uint32_t>(~0U);
        }

        location = locationGather.add(
            nbdspv::OpCompositeConstruct(uvec4Type, editor.MakeId(), {coord, view, prim, zero}));
      }
    }
    else if(stage == ShaderStage::Geometry)
    {
      nbdspv::Id prim = fetchOrAddGlobalInput("rdoc_primitiveIndex", ShaderBuiltin::PrimitiveIndex,
                                              nbdspv::BuiltIn::PrimitiveId, uint32Type, true);

      nbdspv::Id view;

      // only search for the view index is the multiview capability is declared, otherwise it's
      // invalid and we just set 0. Valid for both Vertex and Pixel shaders
      if(editor.HasCapability(nbdspv::Capability::MultiView))
      {
        view = fetchOrAddGlobalInput("rdoc_viewIndex", ShaderBuiltin::MultiViewIndex,
                                     nbdspv::BuiltIn::ViewIndex, uint32Type, true);
      }
      else
      {
        view = editor.AddConstantImmediate<uint32_t>(0U);
      }

      location = locationGather.add(
          nbdspv::OpCompositeConstruct(uvec4Type, editor.MakeId(), {prim, view, zero, zero}));
    }
    else
    {
      NBDWARN("No identifier stored for %s stage", ToStr(stage).c_str());
      location = locationGather.add(
          nbdspv::OpCompositeConstruct(uvec4Type, editor.MakeId(), {zero, zero, zero, zero}));
    }

    locationGather.add(nbdspv::OpStore(printfLocationVar, location));
  }

  if(!newGlobals.empty())
  {
    nbdspv::Iter it = editor.GetEntry(entryID);

    NBDASSERT(it.opcode() == nbdspv::Op::EntryPoint);

    nbdspv::OpEntryPoint entry(it);

    editor.Remove(it);

    entry.iface.append(newGlobals);

    editor.AddOperation(it, entry);
  }

  nbdspv::Id debugPrintfSet = editor.HasExtInst("NonSemantic.DebugPrintf");

  nbdspv::TypeToIds<nbdspv::FunctionType> funcTypes = editor.GetTypes<nbdspv::FunctionType>();

  // functions that have been patched with annotation & extra function parameters if needed
  std::set<nbdspv::Id> patchedFunctions;

  // functions we need to patch, with the indices of which parameters have bindings coming along
  // with
  std::map<nbdspv::Id, nbdarray<size_t>> functionPatchQueue;

  // start with the entry point, with no parameters to patch
  functionPatchQueue[entryID] = {};

  // now keep patching functions until we have no more to patch
  while(!functionPatchQueue.empty())
  {
    nbdspv::Id funcId;
    nbdarray<size_t> patchArgIndices;

    {
      auto it = functionPatchQueue.begin();
      funcId = functionPatchQueue.begin()->first;
      patchArgIndices = functionPatchQueue.begin()->second;
      functionPatchQueue.erase(it);

      patchedFunctions.insert(funcId);
    }

    nbdspv::Iter it = editor.GetID(funcId);

    NBDASSERT(it.opcode() == nbdspv::Op::Function);

    if(!patchArgIndices.empty())
    {
      nbdspv::OpFunction func(it);

      // find the function's type declaration, add the necessary arguments, redeclare and patch it
      for(const nbdspv::TypeToId<nbdspv::FunctionType> &funcType : funcTypes)
      {
        if(funcType.second == func.functionType)
        {
          nbdspv::FunctionType patchedFuncType = funcType.first;
          for(size_t i = 0; i < patchArgIndices.size(); i++)
            patchedFuncType.argumentIds.push_back(indexOffsetType);

          nbdspv::Id newFuncTypeID = editor.DeclareType(patchedFuncType);

          // re-fetch the iterator as it might have moved with the type declaration
          it = editor.GetID(funcId);

          // change the declared function type
          func.functionType = newFuncTypeID;

          editor.PreModify(it);

          it = func;

          editor.PostModify(it);

          break;
        }
      }
    }

    ++it;

    // onto the OpFunctionParameters. First allocate IDs for all our new function parameters
    nbdarray<nbdspv::Id> patchedParamIDs;
    for(size_t i = 0; i < patchArgIndices.size(); i++)
      patchedParamIDs.push_back(editor.MakeId());

    size_t argIndex = 0;
    size_t watchIndex = 0;
    while(it.opcode() == nbdspv::Op::FunctionParameter)
    {
      nbdspv::OpFunctionParameter param(it);

      // if this is a parameter we're patching, add it into varLookup
      if(watchIndex < patchArgIndices.size() && patchArgIndices[watchIndex] == argIndex)
      {
        // when we see use of this parameter, patch it using the added parameter
        varLookup[param.result] = patchedParamIDs[watchIndex];
        // watch for the next argument
        watchIndex++;
      }

      argIndex++;
      ++it;
    }

    // we're past the existing function parameters, now declare our new ones
    for(size_t i = 0; i < patchedParamIDs.size(); i++)
    {
      editor.AddOperation(it, nbdspv::OpFunctionParameter(indexOffsetType, patchedParamIDs[i]));
      ++it;
    }

    // continue to the first label so we can insert things at the start of the entry point
    for(; it; ++it)
    {
      if(it.opcode() == nbdspv::Op::Label)
      {
        ++it;
        break;
      }
    }

    // skip past any local variables
    while(it.opcode() == nbdspv::Op::Variable || it.opcode() == nbdspv::Op::Line ||
          it.opcode() == nbdspv::Op::NoLine)
      ++it;

    if(funcId == entryID)
      editor.AddOperations(it, locationGather);

    // now patch accesses in the function body
    for(; it; ++it)
    {
      // finish when we hit the end of the function
      if(it.opcode() == nbdspv::Op::FunctionEnd)
        break;

      // if we see an OpCopyObject, just add it to the map pointing to the same value
      if(it.opcode() == nbdspv::Op::CopyObject)
      {
        nbdspv::OpCopyObject copy(it);

        // is this a var we want to snoop?
        auto varIt = varLookup.find(copy.operand);
        if(varIt != varLookup.end())
        {
          varLookup[copy.result] = varIt->second;
        }
      }

      if(it.opcode() == nbdspv::Op::FunctionCall)
      {
        nbdspv::OpFunctionCall call(it);

        // check if any of the variables being passed are ones we care about. Accumulate the added
        // parameters
        nbdarray<uint32_t> funccall;
        nbdarray<size_t> patchArgs;

        // examine each argument to see if it's one we care about
        for(size_t i = 0; i < call.arguments.size(); i++)
        {
          // if this param we're snooping then pass our offset - whether it's a constant or a
          // function
          // argument itself - into the function call
          auto varIt = varLookup.find(call.arguments[i]);
          if(varIt != varLookup.end())
          {
            funccall.push_back(varIt->second.value());
            patchArgs.push_back(i);
          }
        }

        // if we have parameters to patch, replace the function call
        if(!funccall.empty())
        {
          // prepend all the existing words
          for(size_t i = 1; i < it.size(); i++)
            funccall.insert(i - 1, it.word(i));

          nbdspv::Iter oldCall = it;

          // add our patched call afterwards
          it++;
          editor.AddOperation(it, nbdspv::Operation(nbdspv::Op::FunctionCall, funccall));

          // remove the old call
          editor.Remove(oldCall);
        }

        // if this function isn't marked for patching yet, and isn't patched, queue it
        if(functionPatchQueue[call.function].empty() &&
           patchedFunctions.find(call.function) == patchedFunctions.end())
          functionPatchQueue[call.function] = patchArgs;
      }

      if((it.opcode() == nbdspv::Op::ExtInst || it.opcode() == nbdspv::Op::ExtInstWithForwardRefsKHR) &&
         Vulkan_PrintfFetch())
      {
        nbdspv::OpExtInst extinst(it);
        // is this a printf extinst?
        if(extinst.set == debugPrintfSet)
        {
          uint32_t printfID = idToOffset[extinst.result];

          nbdspv::Id resultConstant = editor.AddConstantDeferred<uint32_t>(printfID);

          PrintfData &format = printfData[printfID];

          {
            nbdspv::OpString str(editor.GetID(nbdspv::Id::fromWord(extinst.params[0])));
            format.user_format = str.string;
            format.effective_format = PatchFormatString(str.string);
          }

          nbdarray<nbdspv::Id> packetWords;

          // pack all the parameters into uint32s
          for(size_t i = 1; i < extinst.params.size(); i++)
          {
            nbdspv::Id printfparam = nbdspv::Id::fromWord(extinst.params[i]);
            nbdspv::Id type = editor.GetIDType(printfparam);

            nbdspv::Iter typeIt = editor.GetID(type);

            // handle vectors, but no other composites
            uint32_t vecDim = 0;
            if(typeIt.opcode() == nbdspv::Op::TypeVector)
            {
              nbdspv::OpTypeVector vec(typeIt);
              vecDim = vec.componentCount;
              type = vec.componentType;
              typeIt = editor.GetID(type);
            }

            nbdspv::Scalar scalarType(typeIt);

            for(uint32_t comp = 0; comp < NBDMAX(1U, vecDim); comp++)
            {
              nbdspv::Id input = printfparam;

              format.argTypes.push_back(scalarType);

              // if the input is a vector, extract the component we're working on
              if(vecDim > 0)
              {
                input = editor.AddOperation(
                    it, nbdspv::OpCompositeExtract(type, editor.MakeId(), input, {comp}));
                it++;
              }

              // handle ints, floats, and bools
              if(typeIt.opcode() == nbdspv::Op::TypeInt)
              {
                nbdspv::OpTypeInt intType(typeIt);

                nbdspv::Id param = input;

                if(intType.signedness)
                {
                  // extend to 32-bit if needed then bitcast to unsigned
                  if(intType.width < 32)
                  {
                    param = editor.AddOperation(
                        it, nbdspv::OpSConvert(int32Type, editor.MakeId(), param));
                    it++;
                  }

                  param = editor.AddOperation(
                      it, nbdspv::OpBitcast(intType.width == 64 ? uint64Type : uint32Type,
                                            editor.MakeId(), param));
                  it++;
                }
                else
                {
                  // just extend to 32-bit if needed
                  if(intType.width < 32)
                  {
                    param = editor.AddOperation(
                        it, nbdspv::OpSConvert(uint32Type, editor.MakeId(), param));
                    it++;
                  }
                }

                // 64-bit integers we now need to split up the words and add them. Otherwise we have
                // a 32-bit uint to add
                if(intType.width == 64)
                {
                  nbdspv::Id lo = editor.AddOperation(
                      it, nbdspv::OpUConvert(uint32Type, editor.MakeId(), param));
                  it++;

                  nbdspv::Id shifted = editor.AddOperation(
                      it, nbdspv::OpShiftRightLogical(uint64Type, editor.MakeId(), param,
                                                      int64wordshift));
                  it++;

                  nbdspv::Id hi = editor.AddOperation(
                      it, nbdspv::OpUConvert(uint32Type, editor.MakeId(), shifted));
                  it++;

                  packetWords.push_back(lo);
                  packetWords.push_back(hi);
                }
                else
                {
                  packetWords.push_back(param);
                }
              }
              else if(typeIt.opcode() == nbdspv::Op::TypeBool)
              {
                packetWords.push_back(
                    editor.AddOperation(it, nbdspv::OpSelect(uint32Type, editor.MakeId(), input,
                                                             truePrintfValue, falsePrintfValue)));
                it++;
              }
              else if(typeIt.opcode() == nbdspv::Op::TypeFloat)
              {
                nbdspv::OpTypeFloat floatType(typeIt);

                nbdspv::Id param = input;

                // if it's not at least a float, upconvert. We don't convert to doubles since that
                // would require double capability
                if(floatType.width < 32)
                {
                  param =
                      editor.AddOperation(it, nbdspv::OpFConvert(f32Type, editor.MakeId(), param));
                  it++;
                }

                if(floatType.width == 64)
                {
                  // for doubles we use the GLSL unpack operation
                  nbdspv::Id unpacked = editor.AddOperation(
                      it, nbdspv::OpGLSL450(uvec2Type, editor.MakeId(), glsl450,
                                            nbdspv::GLSLstd450::UnpackDouble2x32, {param}));

                  // then extract the components
                  nbdspv::Id lo = editor.AddOperation(
                      it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), unpacked, {0}));
                  it++;

                  nbdspv::Id hi = editor.AddOperation(
                      it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), unpacked, {1}));
                  it++;

                  packetWords.push_back(lo);
                  packetWords.push_back(hi);
                }
                else
                {
                  // otherwise we bitcast to uint32
                  param =
                      editor.AddOperation(it, nbdspv::OpBitcast(uint32Type, editor.MakeId(), param));
                  it++;

                  packetWords.push_back(param);
                }
              }
              else
              {
                NBDERR("Unexpected type of operand to printf %s, ignoring",
                       ToStr(typeIt.opcode()).c_str());
              }
            }
          }

          format.payloadWords = packetWords.size();

          // pack header uint32
          nbdspv::Id header =
              editor.AddOperation(it, nbdspv::OpBitwiseOr(uint32Type, editor.MakeId(),
                                                          shaderStageConstant, resultConstant));
          it++;

          packetWords.insert(0, header);

          // load the location out of the global where we put it
          nbdspv::Id location =
              editor.AddOperation(it, nbdspv::OpLoad(uvec4Type, editor.MakeId(), printfLocationVar));
          it++;

          // extract each component and add it as a new word after the header
          packetWords.insert(
              1, editor.AddOperation(
                     it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), location, {0})));
          it++;
          packetWords.insert(
              2, editor.AddOperation(
                     it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), location, {1})));
          it++;
          packetWords.insert(
              3, editor.AddOperation(
                     it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), location, {2})));
          it++;
          packetWords.insert(
              4, editor.AddOperation(
                     it, nbdspv::OpCompositeExtract(uint32Type, editor.MakeId(), location, {3})));
          it++;

          nbdspv::Id counterptr;

          if(IsBDA(storageMode))
          {
            // make a pointer out of the buffer address
            // uint32_t *bufptr = (uint32_t *)(ptr+0)
            counterptr = MakeOffsettedPointer<uintvulkanmax_t>(
                editor, it, uint32ptrtype, carryStructType, feedbackBuffer.second, nbdspv::Id());

            it++;
          }
          else
          {
            // accesschain to get the pointer we'll atomic into.
            // accesschain is 0 to access rtarray (first member) then zero for the first array index
            // uint32_t *bufptr = (uint32_t *)&buf.printfWords[ssboindex];
            counterptr = editor.AddOperation(
                it, nbdspv::OpAccessChain(uint32ptrtype, editor.MakeId(), feedbackBuffer.second,
                                          {printfArrayOffset, zero}));
            it++;
          }

          nbdspv::Id packetSize = editor.AddConstantDeferred<uint32_t>((uint32_t)packetWords.size());

          // atomically reserve enough space
          nbdspv::Id idx =
              editor.AddOperation(it, nbdspv::OpAtomicIAdd(uint32Type, editor.MakeId(), counterptr,
                                                           scope, semantics, packetSize));
          it++;

          // clamp to the buffer size so we don't overflow
          idx = editor.AddOperation(
              it, nbdspv::OpGLSL450(uint32Type, editor.MakeId(), glsl450, nbdspv::GLSLstd450::UMin,
                                    {idx, maxPrintfWordOffset}));
          it++;

          if(IsBDA(storageMode))
          {
            // convert to an offset value (upconverting as needed, indexOffsetType is always the
            // largest uint type)
            idx = editor.AddOperation(it, nbdspv::OpUConvert(indexOffsetType, editor.MakeId(), idx));
            it++;

            // the index is in words, so multiply by the increment to get a byte offset
            nbdspv::Id byteOffset = editor.AddOperation(
                it, nbdspv::OpIMul(indexOffsetType, editor.MakeId(), idx, printfIncrement));
            it++;

            for(nbdspv::Id word : packetWords)
            {
              // we pre-increment idx because it starts from 0 but we want to write into words
              // starting from [1] to leave the counter itself alone.
              byteOffset = editor.AddOperation(
                  it, nbdspv::OpIAdd(indexOffsetType, editor.MakeId(), byteOffset, printfIncrement));
              it++;

              nbdspv::Id ptr = MakeOffsettedPointer<uintvulkanmax_t>(
                  editor, it, uint32ptrtype, carryStructType, feedbackBuffer.second, byteOffset);
              it++;

              editor.AddOperation(it, nbdspv::OpStore(ptr, word, memoryAccess));
              it++;
            }
          }
          else
          {
            for(nbdspv::Id word : packetWords)
            {
              // we pre-increment idx because it starts from 0 but we want to write into words
              // starting from [1] to leave the counter itself alone.
              idx = editor.AddOperation(
                  it, nbdspv::OpIAdd(uint32Type, editor.MakeId(), idx, printfIncrement));
              it++;

              nbdspv::Id ptr = editor.AddOperation(
                  it, nbdspv::OpAccessChain(uint32ptrtype, editor.MakeId(), feedbackBuffer.second,
                                            {printfArrayOffset, idx}));
              it++;

              editor.AddOperation(it, nbdspv::OpStore(ptr, word));
              it++;
            }
          }

          // no it++ here, it will happen implicitly on loop continue
        }
      }

      // if we see an access chain of a variable we're snooping, save out the result
      if(it.opcode() == nbdspv::Op::AccessChain || it.opcode() == nbdspv::Op::InBoundsAccessChain)
      {
        nbdspv::OpAccessChain chain(it);
        chain.op = it.opcode();

        // is this a var we want to snoop?
        auto varIt = varLookup.find(chain.base);
        if(varIt != varLookup.end())
        {
          // multi-dimensional arrays of descriptors is not allowed - however an access chain could
          // be longer than 5 words (1 index). Think of the case of a uniform buffer where the first
          // index goes into the descriptor array, and further indices go inside the uniform buffer
          // members.
          NBDASSERT(chain.indexes.size() >= 1, chain.indexes.size());

          nbdspv::Id index = chain.indexes[0];

          // patch after the access chain
          it++;

          // upcast the index to our target uint size for indexing/offsetting
          {
            nbdspv::Id indexType = editor.GetIDType(index);

            if(indexType == nbdspv::Id())
            {
              NBDERR("Unknown type for ID %u, defaulting to uint32_t", index.value());
              indexType = uint32Type;
            }

            nbdspv::Scalar indexTypeData = nbdspv::scalar<uint32_t>();
            auto indexTypeIt = intTypeLookup.find(indexType);

            if(indexTypeIt != intTypeLookup.end())
            {
              indexTypeData = indexTypeIt->second;
            }
            else
            {
              NBDERR("Unknown index type ID %u, defaulting to uint32_t", indexType.value());
            }

            // if it's signed, bitcast it to unsigned
            if(indexTypeData.signedness)
            {
              indexTypeData.signedness = false;

              index = editor.AddOperation(
                  it, nbdspv::OpBitcast(editor.DeclareType(indexTypeData), editor.MakeId(), index));
              it++;
            }

            // if it's not wide enough, uconvert expand it
            if(indexTypeData.width != targetIndexWidth)
            {
              nbdspv::Id extendedtype =
                  editor.DeclareType(nbdspv::Scalar(nbdspv::Op::TypeInt, targetIndexWidth, false));
              index =
                  editor.AddOperation(it, nbdspv::OpUConvert(extendedtype, editor.MakeId(), index));
              it++;
            }
          }

          nbdspv::Id bufptr;

          if(IsBDA(storageMode))
          {
            // convert the constant embedded device address to a pointer

            // shift the index since this is a byte offset
            // shiftedindex = index << uint32shift
            nbdspv::Id shiftedindex = editor.AddOperation(
                it, nbdspv::OpShiftLeftLogical(indexOffsetType, editor.MakeId(), index, uint32shift));
            it++;

            // add the index on top of that
            // offsetaddr = bindingOffset + shiftedindex
            nbdspv::Id offsetaddr = editor.AddOperation(
                it, nbdspv::OpIAdd(indexOffsetType, editor.MakeId(), varIt->second, shiftedindex));
            it++;

            // clamp the offset address to the maximum byte offset. If the user is reading out of
            // bounds, don't write out of bounds.
            {
              nbdspv::Id clampedtype =
                  editor.DeclareType(nbdspv::Scalar(nbdspv::Op::TypeInt, targetIndexWidth, false));
              offsetaddr = editor.AddOperation(
                  it, nbdspv::OpGLSL450(clampedtype, editor.MakeId(), glsl450,
                                        nbdspv::GLSLstd450::UMin, {offsetaddr, maxSlotByteOffsetID}));
              it++;
            }

            // make a pointer out of it
            // uint32_t *bufptr = (uint32_t *)(ptr + offsetaddr)
            bufptr = MakeOffsettedPointer<uintvulkanmax_t>(
                editor, it, uint32ptrtype, carryStructType, feedbackBuffer.second, offsetaddr);
            it++;
          }
          else
          {
            // accesschain into the SSBO, by adding the base offset for this var onto the index

            // add the index to this binding's base index
            // ssboindex = bindingOffset + index
            nbdspv::Id ssboindex = editor.AddOperation(
                it, nbdspv::OpIAdd(uint32Type, editor.MakeId(), index, varIt->second));
            it++;

            // clamp the ssboindex to the maximum slot. If the user is reading out of bounds, don't
            // write out of bounds.
            {
              nbdspv::Id clampedtype =
                  editor.DeclareType(nbdspv::Scalar(nbdspv::Op::TypeInt, targetIndexWidth, false));
              ssboindex = editor.AddOperation(
                  it, nbdspv::OpGLSL450(clampedtype, editor.MakeId(), glsl450,
                                        nbdspv::GLSLstd450::UMin, {ssboindex, maxSlotID}));
              it++;
            }

            // accesschain to get the pointer we'll atomic into.
            // accesschain is 0 to access rtarray (first member) then ssboindex for array index
            // uint32_t *bufptr = (uint32_t *)&buf.rtarray[ssboindex];
            bufptr = editor.AddOperation(
                it, nbdspv::OpAccessChain(uint32ptrtype, editor.MakeId(), feedbackBuffer.second,
                                          {rtarrayOffset, ssboindex}));
            it++;
          }

          // atomically set the uint32 that's pointed to
          editor.AddOperation(it, nbdspv::OpAtomicUMax(uint32Type, editor.MakeId(), bufptr, scope,
                                                       semantics, usedValue));

          // no it++ here, it will happen implicitly on loop continue
        }
      }
    }
  }
}

void VulkanReplay::ClearFeedbackCache()
{
  m_BindlessFeedback.clear();
}

bool VulkanReplay::FetchShaderFeedback(uint32_t eventId)
{
  if(m_BindlessFeedback.find(eventId) != m_BindlessFeedback.end())
    return false;

  if(!Vulkan_BindlessFeedback())
    return false;

  // create it here so we won't re-run any code if the event is re-selected. We'll mark it as valid
  // if it actually has any data in it later.
  VKDynamicShaderFeedback &result = m_BindlessFeedback[eventId];

  const VulkanRenderState &state = m_pDriver->m_RenderState;
  VulkanCreationInfo &creationInfo = m_pDriver->m_CreationInfo;

  const ActionDescription *action = m_pDriver->GetAction(eventId);

  if(action == NULL ||
     !(action->flags & (ActionFlags::Dispatch | ActionFlags::MeshDispatch | ActionFlags::Drawcall)))
  {
    // deliberately show no bindings as used for non-draws
    result.valid = true;
    return false;
  }

  result.compute = bool(action->flags & ActionFlags::Dispatch);

  const VulkanStatePipeline &pipe = result.compute ? state.compute : state.graphics;

  if(pipe.pipeline == ResourceId())
  {
    result.valid = true;
    return false;
  }

  const VulkanCreationInfo::Pipeline &pipeInfo = creationInfo.m_Pipeline[pipe.pipeline];

  bool usesPrintf = false;

  for(uint32_t i = 0; i < NumShaderStages; i++)
  {
    if(pipeInfo.shaders[i].stage == ShaderStage::Count)
      continue;

    usesPrintf |= pipeInfo.shaders[i].patchData->usesPrintf;
  }

  const bool hasGeomOrMesh = pipeInfo.shaders[(size_t)ShaderStage::Geometry].module != ResourceId() ||
                             pipeInfo.shaders[(size_t)ShaderStage::Mesh].module != ResourceId();

  const bool usePrimitiveID =
      !hasGeomOrMesh && m_pDriver->GetDeviceEnabledFeatures().geometryShader != VK_FALSE;

  const bool usesMultiview =
      state.GetRenderPass() != ResourceId()
          ? creationInfo.m_RenderPass[state.GetRenderPass()].subpasses[state.subpass].multiviews.size() >
                1
          : pipeInfo.viewMask != 0;

  BindlessFeedbackData feedbackData;

  if(usesPrintf)
  {
    // reserve some space at the start for an atomic offset counter then the buffer size, and an
    // overflow section for any clamped messages
    feedbackData.feedbackStorageSize += 16 + Vulkan_Debug_PrintfBufferSize() + 1024;
  }

  const ShaderReflection *stageRefls[NumShaderStages] = {};

  {
    const nbdarray<VulkanRenderState::DescriptorBuffer> &descBufs = state.descBufs;
    const nbdarray<VulkanStatePipeline::DescriptorAndOffsets> &descSets =
        (result.compute ? state.compute.descSets : state.graphics.descSets);

    nbdarray<const DescSetLayout *> descLayouts;
    for(size_t set = 0; set < pipeInfo.descSetLayouts.size(); set++)
      descLayouts.push_back(&creationInfo.m_DescSetLayout[pipeInfo.descSetLayouts[set]]);

    auto processBinding = [this, &descLayouts, &descBufs, &descSets, &feedbackData](
                              ShaderStage stage, DescriptorType type, bool inputAttachment,
                              uint16_t index, uint32_t bindset, uint32_t bind, uint32_t arraySize) {
      // only process array bindings
      if(arraySize <= 1)
        return;

      BindKey key;
      key.stage = stage;
      key.index.category = CategoryForDescriptorType(type);
      key.index.index = index;
      key.index.arrayElement = 0;

      if(bindset >= descLayouts.size() || !descLayouts[bindset] || bindset >= descSets.size() ||
         !descSets[bindset].IsBound())
      {
        NBDERR("Invalid set %u referenced by %s shader", bindset, ToStr(key.stage).c_str());
        return;
      }

      // VkShaderStageFlagBits and ShaderStageMask are identical bit-for-bit.
      if((descLayouts[bindset]->bindings[bind].stageFlags &
          (VkShaderStageFlags)MaskForStage(key.stage)) == 0)
      {
        // this might be deliberate if the binding is never actually used dynamically, only
        // statically used bindings must be declared
        return;
      }

      DescriptorAccess access;
      access.stage = key.stage;
      access.type = type;
      access.index = index;

      if(descSets[bindset].descSet == ResourceId())
      {
        ResourceId id;
        uint64_t offs = 0;
        m_pDriver->GetResIDFromAddr(descBufs[descSets[bindset].descBufferIdx].address, id, offs);
        access.descriptorStore = id;
        access.byteOffset += uint32_t(offs + descSets[bindset].descBufferOffset) +
                             descLayouts[bindset]->bindings[bind].elemOffset;
        access.byteSize =
            GetDescriptorSizeOfBind(m_pDriver->GetResourceManager(), descLayouts[bindset]->bindings,
                                    descLayouts[bindset]->mutableBitmasks, bind);

        if(descLayouts[bindset]->bindings[bind].variableSize || arraySize == ~0U)
        {
          arraySize = uint32_t((m_pDriver->m_CreationInfo.m_Buffer[id].size - access.byteOffset) /
                               access.byteSize);
        }
      }
      else
      {
        ResourceId descSet = descSets[bindset].descSet;

        if(bind >= descLayouts[bindset]->bindings.size())
        {
          NBDERR("Invalid binding %u in set %u referenced by %s shader", bind, bindset,
                 ToStr(key.stage).c_str());
          return;
        }

        if(descLayouts[bindset]->bindings[bind].variableSize)
        {
          auto it = m_pDriver->m_DescriptorSetState.find(descSet);
          if(it != m_pDriver->m_DescriptorSetState.end())
            arraySize = it->second.data.variableDescriptorCount;
        }
        else if(arraySize == ~0U)
        {
          // if the array was unbounded, clamp it to the size of the descriptor set
          arraySize = descLayouts[bindset]->bindings[bind].descriptorCount;
        }

        access.descriptorStore = descSet;
        access.byteOffset =
            descLayouts[bindset]->bindings[bind].elemOffset + descLayouts[bindset]->inlineByteSize;
        access.byteSize = 1;
      }

      feedbackData.offsetMap[key] = {feedbackData.feedbackStorageSize, arraySize, access};

      feedbackData.feedbackStorageSize += arraySize * sizeof(uint32_t);
    };

    for(const VulkanCreationInfo::ShaderEntry &sh : pipeInfo.shaders)
    {
      if(!sh.refl)
        continue;

      stageRefls[(uint32_t)sh.refl->stage] = sh.refl;

      for(uint32_t i = 0; i < sh.refl->constantBlocks.size(); i++)
        processBinding(sh.refl->stage, DescriptorType::ConstantBuffer, false, i & 0xffff,
                       sh.refl->constantBlocks[i].fixedBindSetOrSpace,
                       sh.refl->constantBlocks[i].fixedBindNumber,
                       sh.refl->constantBlocks[i].bindArraySize);

      for(uint32_t i = 0; i < sh.refl->samplers.size(); i++)
        processBinding(sh.refl->stage, DescriptorType::Sampler, false, i & 0xffff,
                       sh.refl->samplers[i].fixedBindSetOrSpace,
                       sh.refl->samplers[i].fixedBindNumber, sh.refl->samplers[i].bindArraySize);

      for(uint32_t i = 0; i < sh.refl->readOnlyResources.size(); i++)
        processBinding(sh.refl->stage, sh.refl->readOnlyResources[i].descriptorType,
                       sh.refl->readOnlyResources[i].isInputAttachment, i & 0xffff,
                       sh.refl->readOnlyResources[i].fixedBindSetOrSpace,
                       sh.refl->readOnlyResources[i].fixedBindNumber,
                       sh.refl->readOnlyResources[i].bindArraySize);

      for(uint32_t i = 0; i < sh.refl->readWriteResources.size(); i++)
        processBinding(sh.refl->stage, sh.refl->readWriteResources[i].descriptorType, false,
                       i & 0xffff, sh.refl->readWriteResources[i].fixedBindSetOrSpace,
                       sh.refl->readWriteResources[i].fixedBindNumber,
                       sh.refl->readWriteResources[i].bindArraySize);
    }
  }

  uint32_t maxSlot = uint32_t(feedbackData.feedbackStorageSize / sizeof(uint32_t));

  // add some extra padding just in case of out-of-bounds writes
  feedbackData.feedbackStorageSize += 128;

  // if we don't have any array descriptors or printf's to feedback then just return now
  if(feedbackData.offsetMap.empty() && !usesPrintf)
  {
    return false;
  }

  if(!m_pDriver->GetDeviceEnabledFeatures().shaderInt64 &&
     feedbackData.feedbackStorageSize > 0xffff0000U)
  {
    NBDLOG(
        "Feedback buffer is too large for 32-bit addressed maths, and device doesn't support "
        "int64");
    return false;
  }

  if(!result.compute)
  {
    // if we don't have any stores supported at all, we can't do feedback on the graphics pipeline
    if(!m_pDriver->GetDeviceEnabledFeatures().vertexPipelineStoresAndAtomics &&
       !m_pDriver->GetDeviceEnabledFeatures().fragmentStoresAndAtomics)
    {
      return false;
    }
  }

  m_PatchedShaderFeedback.ResizeFeedbackBuffer(m_pDriver, feedbackData.feedbackStorageSize);

  nbdarray<VkDescriptorSetLayoutBinding> newBindings = {
      {
          0,
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          1,
          VkShaderStageFlags(result.compute ? VK_SHADER_STAGE_COMPUTE_BIT
                                            : VK_SHADER_STAGE_ALL_GRAPHICS),
          NULL,
      },
  };

  NBDASSERTMSG("ShaderFeedbackReservedBindings is wrong",
               newBindings.size() == ShaderFeedbackReservedBindings, newBindings.size());

  // make copy of state to draw from
  VulkanRenderState modifiedstate = state;

  AddedDescriptorData patchedBufferdata =
      PrepareExtraBufferDescriptor(modifiedstate, result.compute, newBindings, false);

  if(patchedBufferdata.empty())
    return false;

  if(!patchedBufferdata.descSets.empty())
    m_PatchedShaderFeedback.FeedbackBuffer.WriteDescriptor(Unwrap(patchedBufferdata.descSets[0]), 0,
                                                           0);

  std::map<uint32_t, PrintfData> printfData[NumShaderStages];

  auto patchCallback = [this, &printfData, &feedbackData, pipeInfo, maxSlot, usePrimitiveID,
                        usesMultiview](
                           const AddedDescriptorData &patchedBufferdata, VkShaderStageFlagBits stage,
                           const char *entryName, const nbdarray<uint32_t> &origSpirv,
                           nbdarray<uint32_t> &modSpirv, const VkSpecializationInfo *&specInfo) {
    // can't patch if stores aren't supported
    if((stage & VK_SHADER_STAGE_FRAGMENT_BIT) != 0)
    {
      if(!m_pDriver->GetDeviceEnabledFeatures().fragmentStoresAndAtomics)
        return false;
    }
    else if((stage & (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                      VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT)) !=
            0)
    {
      if(!m_pDriver->GetDeviceEnabledFeatures().vertexPipelineStoresAndAtomics)
        return false;
    }

    int idx = StageIndex(stage);

    modSpirv = origSpirv;

    static const nbdstr filename[NumShaderStages] = {
        "bindless_vertex.spv",   "bindless_hull.spv",  "bindless_domain.spv",
        "bindless_geometry.spv", "bindless_pixel.spv", "bindless_compute.spv",
        "bindless_task.spv",     "bindless_mesh.spv",
    };

    if(!Vulkan_Debug_FeedbackDumpDirPath().empty())
      FileIO::WriteAll(Vulkan_Debug_FeedbackDumpDirPath() + "/before_" + filename[idx], modSpirv);

    if(m_pDriver->GetDeviceEnabledFeatures().shaderInt64)
    {
      AnnotateShader<uint64_t>(*pipeInfo.shaders[idx].refl, *pipeInfo.shaders[idx].patchData,
                               ShaderStage(idx), entryName, feedbackData.offsetMap, maxSlot,
                               usePrimitiveID, m_PatchedShaderFeedback.FeedbackBuffer.Address(),
                               m_StorageMode, usesMultiview, modSpirv, printfData[idx]);
    }
    else
    {
      AnnotateShader<uint32_t>(*pipeInfo.shaders[idx].refl, *pipeInfo.shaders[idx].patchData,
                               ShaderStage(idx), entryName, feedbackData.offsetMap, maxSlot,
                               usePrimitiveID, m_PatchedShaderFeedback.FeedbackBuffer.Address(),
                               m_StorageMode, usesMultiview, modSpirv, printfData[idx]);
    }

    if(!Vulkan_Debug_FeedbackDumpDirPath().empty())
      FileIO::WriteAll(Vulkan_Debug_FeedbackDumpDirPath() + "/after_" + filename[idx], modSpirv);

    return true;
  };

  PrepareStateForPatchedShader(patchedBufferdata, modifiedstate, result.compute, patchCallback);

  if(!RunFeedbackAction(feedbackData.feedbackStorageSize, action, modifiedstate))
    return false;

  bytebuf data;
  GetDebugManager()->GetBufferData(m_PatchedShaderFeedback.FeedbackBuffer, 0, 0, data);

  for(auto it = feedbackData.offsetMap.begin(); it != feedbackData.offsetMap.end(); ++it)
  {
    uint32_t *readbackData = (uint32_t *)(data.data() + it->second.offset);

    DescriptorAccess access = it->second.access;

    for(uint32_t i = 0; i < it->second.numEntries; i++)
    {
      if(readbackData[i])
      {
        access.arrayElement = i;

        result.access.push_back(access);
      }

      access.byteOffset += access.byteSize;
    }
  }

  result.valid = true;

  uint32_t *printfBuf = (uint32_t *)data.data();
  uint32_t *printfBufEnd = (uint32_t *)(data.data() + Vulkan_Debug_PrintfBufferSize());
  if(usesPrintf && *printfBuf > 0)
  {
    uint32_t wordsNeeded = *printfBuf;

    if(wordsNeeded > Vulkan_Debug_PrintfBufferSize())
    {
      NBDLOG("printf buffer overflowed, needed %u bytes but printf buffer is only %u bytes",
             wordsNeeded * 4, Vulkan_Debug_PrintfBufferSize());
    }

    printfBuf++;

    while(*printfBuf && printfBuf < printfBufEnd)
    {
      ShaderStage stage = ShaderStage((*printfBuf) >> ShaderStageHeaderBitShift);
      uint32_t printfID = *printfBuf & 0xfffffffU;

      printfBuf++;

      if(stage < ShaderStage::Count)
      {
        auto it = printfData[(uint32_t)stage].find(printfID);
        if(it == printfData[(uint32_t)stage].end())
        {
          NBDERR("Error parsing DebugPrintf buffer, unexpected printf ID %x from header %x",
                 printfID, *printfBuf);
          break;
        }

        uint32_t *location = printfBuf;

        printfBuf += 4;

        const PrintfData &fmt = it->second;

        ShaderPrintfArgs args(printfBuf, fmt);

        printfBuf += fmt.payloadWords;

        // this message overflowed, don't process it
        if(printfBuf >= printfBufEnd)
          break;

        ShaderMessage msg;

        msg.stage = stage;

        const VulkanCreationInfo::ShaderEntry &sh = pipeInfo.shaders[(uint32_t)stage];

        {
          VulkanCreationInfo::ShaderModule &mod = creationInfo.m_ShaderModule[sh.module];
          VulkanCreationInfo::ShaderModuleReflection &modrefl =
              mod.GetReflection(stage, sh.entryPoint, pipe.pipeline);
          modrefl.PopulateDisassembly(mod.spirv);

          const std::map<size_t, uint32_t> &instructionLines = modrefl.instructionLines;

          auto instit = instructionLines.find(printfID);
          if(instit != instructionLines.end())
            msg.disassemblyLine = (int32_t)instit->second;
          else
            msg.disassemblyLine = -1;
        }

        if(stage == ShaderStage::Compute)
        {
          for(int x = 0; x < 3; x++)
          {
            uint32_t threadDimX = sh.refl->dispatchThreadsDimension[x];
            msg.location.compute.workgroup[x] = location[x] / threadDimX;
            msg.location.compute.thread[x] = location[x] % threadDimX;
          }
        }
        else if(stage == ShaderStage::Task)
        {
          for(int x = 0; x < 3; x++)
          {
            uint32_t threadDimX = sh.refl->dispatchThreadsDimension[x];
            msg.location.mesh.taskGroup[x] = location[x] / threadDimX;
            msg.location.mesh.thread[x] = location[x] % threadDimX;
          }
        }
        else if(stage == ShaderStage::Vertex)
        {
          msg.location.vertex.vertexIndex = location[0];
          if(!(action->flags & ActionFlags::Indexed))
          {
            // for non-indexed draws get back to 0-based index
            msg.location.vertex.vertexIndex -= action->vertexOffset;
          }
          // go back to a 0-based instance index
          msg.location.vertex.instance = location[1] - action->instanceOffset;
          msg.location.vertex.view = location[2];
        }
        else if(stage == ShaderStage::Geometry)
        {
          msg.location.geometry.primitive = location[0];
          msg.location.geometry.view = location[1];
        }
        else if(stage == ShaderStage::Mesh)
        {
          for(int x = 0; x < 3; x++)
            msg.location.mesh.meshGroup[x] = location[x];

          uint32_t meshThread = msg.location.mesh.meshGroup[2] >> 16U;
          msg.location.mesh.meshGroup[2] &= 0xffffu;

          msg.location.mesh.thread[0] = meshThread % sh.refl->dispatchThreadsDimension[0];
          msg.location.mesh.thread[1] = (meshThread / sh.refl->dispatchThreadsDimension[0]) %
                                        sh.refl->dispatchThreadsDimension[1];
          msg.location.mesh.thread[2] = meshThread / (sh.refl->dispatchThreadsDimension[0] *
                                                      sh.refl->dispatchThreadsDimension[1]);

          const VulkanCreationInfo::ShaderEntry &tasksh =
              pipeInfo.shaders[(uint32_t)ShaderStage::Task];

          if(tasksh.module == ResourceId())
          {
            msg.location.mesh.taskGroup = {ShaderMeshMessageLocation::NotUsed,
                                           ShaderMeshMessageLocation::NotUsed,
                                           ShaderMeshMessageLocation::NotUsed};
          }
          else
          {
            uint32_t taskGroup = location[3];

            msg.location.mesh.taskGroup[0] = taskGroup % tasksh.refl->dispatchThreadsDimension[0];
            msg.location.mesh.taskGroup[1] = (taskGroup / tasksh.refl->dispatchThreadsDimension[0]) %
                                             tasksh.refl->dispatchThreadsDimension[1];
            msg.location.mesh.taskGroup[2] = taskGroup / (tasksh.refl->dispatchThreadsDimension[0] *
                                                          tasksh.refl->dispatchThreadsDimension[1]);
          }
        }
        else
        {
          msg.location.pixel.x = location[0] >> 16U;
          msg.location.pixel.y = location[0] & 0xffff;
          msg.location.pixel.sample = location[1] >> 16U;
          msg.location.pixel.view = location[1] & 0xffff;
          msg.location.pixel.primitive = location[2];
          if(msg.location.pixel.sample == (~0U >> 16U))
          {
            msg.location.pixel.sample = ~0U;
          }
        }

        msg.message = StringFormat::FmtArgs(fmt.effective_format.c_str(), args);

        if(!args.get_error().empty())
          msg.message = args.get_error() + " in \"" + fmt.user_format + "\"";

        result.messages.push_back(msg);
      }
      else
      {
        NBDERR("Error parsing DebugPrintf buffer, unexpected stage %x from header %x", stage,
               *printfBuf);
        break;
      }
    }
  }

  patchedBufferdata.Free();

  return true;
}

#if ENABLED(ENABLE_UNIT_TESTS)

#undef Always
#undef None

#include "catch/catch.hpp"

TEST_CASE("Test printf format string mangling", "[vulkan]")
{
  SECTION("Vector format expansion")
  {
    CHECK(PatchFormatString("hello %f normal %i string") == "hello %f normal %i string");
    CHECK(PatchFormatString("hello %% normal %2i string") == "hello %% normal %2i string");
    CHECK(PatchFormatString("hello %fv normal %iv string") == "hello %fv normal %iv string");
    CHECK(PatchFormatString("hello %02.3fv normal % 2.fiv string") ==
          "hello %02.3fv normal % 2.fiv string");
    CHECK(PatchFormatString("vector string: %v2f | %v3i") == "vector string: %f, %f | %i, %i, %i");
    CHECK(PatchFormatString("vector with precision: %04.3v4f !") ==
          "vector with precision: %04.3f, %04.3f, %04.3f, %04.3f !");
    CHECK(PatchFormatString("vector at end %v2f") == "vector at end %f, %f");
    CHECK(PatchFormatString("%v3f vector at start") == "%f, %f, %f vector at start");
    CHECK(PatchFormatString("%v2f") == "%f, %f");
    CHECK(PatchFormatString("%v2u") == "%u, %u");
  };

  SECTION("64-bit format twiddling")
  {
    CHECK(PatchFormatString("hello %ul") == "hello %llu");
    CHECK(PatchFormatString("%ul hello") == "%llu hello");
    CHECK(PatchFormatString("%ul") == "%llu");
    CHECK(PatchFormatString("hello %04ul there") == "hello %04llu there");
    CHECK(PatchFormatString("hello %v2ul there") == "hello %llu, %llu there");

    CHECK(PatchFormatString("hello %u l there") == "hello %u l there");

    CHECK(PatchFormatString("%v2u") == "%u, %u");
    CHECK(PatchFormatString("%v2ul") == "%llu, %llu");
  };
};

#endif    // ENABLED(ENABLE_UNIT_TESTS)
