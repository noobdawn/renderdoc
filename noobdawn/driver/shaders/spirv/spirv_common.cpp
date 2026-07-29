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

#include "spirv_common.h"
#include "api/replay/replay_enums.h"
#include "common/common.h"
#include "common/formatting.h"

template <>
nbdstr DoStringise(const nbdspv::Id &el)
{
  uint32_t id;
  NBDCOMPILE_ASSERT(sizeof(el) == sizeof(id), "SPIR-V Id isn't 32-bit!");
  memcpy(&id, &el, sizeof(el));
  return StringFormat::Fmt("%u", id);
}

void nbdspv::Iter::nopRemove(size_t idx, size_t count)
{
  NBDASSERT(idx >= 1);
  size_t oldSize = size();

  if(count == 0)
    count = oldSize - idx;

  // reduce the size of this op
  word(0) = nbdspv::Operation::MakeHeader(opcode(), oldSize - count);

  if(idx + count < oldSize)
  {
    // move any words on the end into the middle, then nop them
    for(size_t i = 0; i < count; i++)
    {
      word(idx + i) = word(idx + count + i);
      word(oldSize - i - 1) = OpNopWord;
    }
  }
  else
  {
    for(size_t i = 0; i < count; i++)
    {
      word(idx + i) = OpNopWord;
    }
  }
}

void nbdspv::Iter::nopRemove()
{
  for(size_t i = 0, sz = size(); i < sz; i++)
    word(i) = OpNopWord;
}

nbdspv::Iter &nbdspv::Iter::operator=(const Operation &op)
{
  size_t newSize = op.size();
  size_t oldSize = size();
  if(newSize > oldSize)
  {
    NBDERR("Can't resize up from %zu to %zu", oldSize, newSize);
    return *this;
  }

  memcpy(&cur(), &op[0], sizeof(uint32_t) * NBDMIN(oldSize, newSize));

  // set remaining words to NOP if we reduced the size
  for(size_t i = newSize; i < oldSize; i++)
    word(i) = OpNopWord;

  return *this;
}

ShaderStage MakeShaderStage(nbdspv::ExecutionModel model)
{
  switch(model)
  {
    case nbdspv::ExecutionModel::Vertex: return ShaderStage::Vertex;
    case nbdspv::ExecutionModel::TessellationControl: return ShaderStage::Tess_Control;
    case nbdspv::ExecutionModel::TessellationEvaluation: return ShaderStage::Tess_Eval;
    case nbdspv::ExecutionModel::Geometry: return ShaderStage::Geometry;
    case nbdspv::ExecutionModel::Fragment: return ShaderStage::Fragment;
    case nbdspv::ExecutionModel::GLCompute: return ShaderStage::Compute;
    case nbdspv::ExecutionModel::TaskEXT: return ShaderStage::Task;
    case nbdspv::ExecutionModel::MeshEXT: return ShaderStage::Mesh;
    case nbdspv::ExecutionModel::RayGenerationKHR: return ShaderStage::RayGen;
    case nbdspv::ExecutionModel::IntersectionKHR: return ShaderStage::Intersection;
    case nbdspv::ExecutionModel::AnyHitKHR: return ShaderStage::AnyHit;
    case nbdspv::ExecutionModel::ClosestHitKHR: return ShaderStage::ClosestHit;
    case nbdspv::ExecutionModel::MissKHR: return ShaderStage::Miss;
    case nbdspv::ExecutionModel::CallableKHR: return ShaderStage::Callable;
    case nbdspv::ExecutionModel::Kernel:
    case nbdspv::ExecutionModel::TaskNV:
    case nbdspv::ExecutionModel::MeshNV:
      // all of these are currently unsupported
      break;
    case nbdspv::ExecutionModel::Invalid:
    case nbdspv::ExecutionModel::Max: break;
  }

  return ShaderStage::Count;
}

ShaderBuiltin MakeShaderBuiltin(ShaderStage stage, const nbdspv::BuiltIn el)
{
  // not complete, might need to expand system attribute list

  switch(el)
  {
    case nbdspv::BuiltIn::Position: return ShaderBuiltin::Position;
    case nbdspv::BuiltIn::PointSize: return ShaderBuiltin::PointSize;
    case nbdspv::BuiltIn::ClipDistance: return ShaderBuiltin::ClipDistance;
    case nbdspv::BuiltIn::CullDistance: return ShaderBuiltin::CullDistance;
    case nbdspv::BuiltIn::VertexId: return ShaderBuiltin::VertexIndex;
    case nbdspv::BuiltIn::InstanceId: return ShaderBuiltin::InstanceIndex;
    case nbdspv::BuiltIn::PrimitiveId: return ShaderBuiltin::PrimitiveIndex;
    case nbdspv::BuiltIn::InvocationId:
    {
      if(stage == ShaderStage::Geometry)
        return ShaderBuiltin::GSInstanceIndex;
      else
        return ShaderBuiltin::OutputControlPointIndex;
    }
    case nbdspv::BuiltIn::Layer: return ShaderBuiltin::RTIndex;
    case nbdspv::BuiltIn::ViewportIndex: return ShaderBuiltin::ViewportIndex;
    case nbdspv::BuiltIn::TessLevelOuter: return ShaderBuiltin::OuterTessFactor;
    case nbdspv::BuiltIn::TessLevelInner: return ShaderBuiltin::InsideTessFactor;
    case nbdspv::BuiltIn::PatchVertices: return ShaderBuiltin::PatchNumVertices;
    case nbdspv::BuiltIn::FragCoord: return ShaderBuiltin::Position;
    case nbdspv::BuiltIn::FrontFacing: return ShaderBuiltin::IsFrontFace;
    case nbdspv::BuiltIn::SampleId: return ShaderBuiltin::MSAASampleIndex;
    case nbdspv::BuiltIn::SamplePosition: return ShaderBuiltin::MSAASamplePosition;
    case nbdspv::BuiltIn::SampleMask: return ShaderBuiltin::MSAACoverage;
    case nbdspv::BuiltIn::FragDepth: return ShaderBuiltin::DepthOutput;
    case nbdspv::BuiltIn::VertexIndex: return ShaderBuiltin::VertexIndex;
    case nbdspv::BuiltIn::InstanceIndex: return ShaderBuiltin::InstanceIndex;
    case nbdspv::BuiltIn::BaseVertex: return ShaderBuiltin::BaseVertex;
    case nbdspv::BuiltIn::BaseInstance: return ShaderBuiltin::BaseInstance;
    case nbdspv::BuiltIn::DrawIndex: return ShaderBuiltin::DrawIndex;
    case nbdspv::BuiltIn::ViewIndex: return ShaderBuiltin::MultiViewIndex;
    case nbdspv::BuiltIn::FragStencilRefEXT: return ShaderBuiltin::StencilReference;
    case nbdspv::BuiltIn::NumWorkgroups: return ShaderBuiltin::DispatchSize;
    case nbdspv::BuiltIn::GlobalInvocationId: return ShaderBuiltin::DispatchThreadIndex;
    case nbdspv::BuiltIn::WorkgroupId: return ShaderBuiltin::GroupIndex;
    case nbdspv::BuiltIn::WorkgroupSize: return ShaderBuiltin::GroupSize;
    case nbdspv::BuiltIn::LocalInvocationIndex: return ShaderBuiltin::GroupFlatIndex;
    case nbdspv::BuiltIn::LocalInvocationId: return ShaderBuiltin::GroupThreadIndex;
    case nbdspv::BuiltIn::TessCoord: return ShaderBuiltin::DomainLocation;
    case nbdspv::BuiltIn::PointCoord: return ShaderBuiltin::PointCoord;
    case nbdspv::BuiltIn::HelperInvocation: return ShaderBuiltin::IsHelper;
    case nbdspv::BuiltIn::SubgroupSize: return ShaderBuiltin::SubgroupSize;
    case nbdspv::BuiltIn::NumSubgroups: return ShaderBuiltin::NumSubgroups;
    case nbdspv::BuiltIn::SubgroupId: return ShaderBuiltin::SubgroupIndexInWorkgroup;
    case nbdspv::BuiltIn::SubgroupLocalInvocationId: return ShaderBuiltin::IndexInSubgroup;
    case nbdspv::BuiltIn::SubgroupEqMask: return ShaderBuiltin::SubgroupEqualMask;
    case nbdspv::BuiltIn::SubgroupGeMask: return ShaderBuiltin::SubgroupGreaterEqualMask;
    case nbdspv::BuiltIn::SubgroupGtMask: return ShaderBuiltin::SubgroupGreaterMask;
    case nbdspv::BuiltIn::SubgroupLeMask: return ShaderBuiltin::SubgroupLessEqualMask;
    case nbdspv::BuiltIn::SubgroupLtMask: return ShaderBuiltin::SubgroupLessMask;
    case nbdspv::BuiltIn::DeviceIndex: return ShaderBuiltin::DeviceIndex;
    case nbdspv::BuiltIn::FullyCoveredEXT: return ShaderBuiltin::IsFullyCovered;
    case nbdspv::BuiltIn::BaryCoordKHR: return ShaderBuiltin::Barycentrics;
    case nbdspv::BuiltIn::FragSizeEXT: return ShaderBuiltin::FragAreaSize;
    case nbdspv::BuiltIn::FragInvocationCountEXT: return ShaderBuiltin::FragInvocationCount;
    case nbdspv::BuiltIn::PrimitivePointIndicesEXT: return ShaderBuiltin::OutputIndices;
    case nbdspv::BuiltIn::PrimitiveLineIndicesEXT: return ShaderBuiltin::OutputIndices;
    case nbdspv::BuiltIn::PrimitiveTriangleIndicesEXT: return ShaderBuiltin::OutputIndices;
    case nbdspv::BuiltIn::CullPrimitiveEXT: return ShaderBuiltin::CullPrimitive;
    case nbdspv::BuiltIn::PrimitiveShadingRateKHR:
    case nbdspv::BuiltIn::ShadingRateKHR: return ShaderBuiltin::PackedFragRate;
    default: break;
  }

  NBDWARN("Couldn't map SPIR-V built-in %s to known built-in", ToStr(el).c_str());

  return ShaderBuiltin::Undefined;
}

namespace nbdspv
{

bool ManualForEachID(const ConstIter &it, const std::function<void(Id, bool)> &callback)
{
  switch(it.opcode())
  {
    case nbdspv::Op::Switch:
      // Include just the selector
      callback(Id::fromWord(it.word(1)), false);
      return true;
    default:
      // unhandled
      return false;
  }
}

};    // namespace nbdspv
