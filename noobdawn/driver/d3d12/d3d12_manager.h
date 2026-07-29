/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
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

#include "common/wrapped_pool.h"
#include "core/core.h"
#include "core/gpu_address_range_tracker.h"
#include "core/intervals.h"
#include "core/resource_manager.h"
#include "core/sparse_page_table.h"
#include "driver/d3d12/d3d12_common.h"
#include "serialise/serialiser.h"

enum D3D12ResourceType
{
  Resource_Unknown = 0,
  Resource_Device,
  Resource_CommandAllocator,
  Resource_CommandQueue,
  Resource_CommandSignature,
  Resource_DescriptorHeap,
  Resource_Fence,
  Resource_Heap,
  Resource_PipelineState,
  Resource_QueryHeap,
  Resource_Resource,
  Resource_GraphicsCommandList,
  Resource_RootSignature,
  Resource_PipelineLibrary,
  Resource_ProtectedResourceSession,
  Resource_ShaderCacheSession,
  Resource_AccelerationStructure,
  Resource_StateObject
};

DECLARE_REFLECTION_ENUM(D3D12ResourceType);

class WrappedID3D12DescriptorHeap;

// squeeze the descriptor a bit so that the D3D12Descriptor struct fits in 64 bytes
struct D3D12_UNORDERED_ACCESS_VIEW_DESC_SQUEEZED
{
  // pull up and compress down to 1 byte the enums/flags that don't have any larger values
  uint8_t Format;
  uint8_t ViewDimension;
  uint8_t BufferFlags;

  // 1 more bytes here

  uint32_t BufferStride;

  union
  {
    struct D3D12_BUFFER_UAV_SQUEEZED
    {
      UINT64 FirstElement;
      UINT NumElements;
      UINT64 CounterOffsetInBytes;
    } Buffer;
    struct D3D12_BUFFER_UAV_BYTE_OFFSET_SQUEEZED
    {
      UINT64 Offset;
      UINT64 Size;
      UINT64 CounterOffsetInBytes;
    } BufferByteOffset;
    D3D12_TEX1D_UAV Texture1D;
    D3D12_TEX1D_ARRAY_UAV Texture1DArray;
    D3D12_TEX2D_UAV Texture2D;
    D3D12_TEX2D_ARRAY_UAV Texture2DArray;
    D3D12_TEX3D_UAV Texture3D;
  };

  void Init(const D3D12_UNORDERED_ACCESS_VIEW_DESC &desc)
  {
    Format = (uint8_t)desc.Format;
    ViewDimension = (uint8_t)desc.ViewDimension;

    // all but buffer elements should fit in 2 UINT64s, so when we copy Offset and Size (first two
    // elements in BufferByteOffset) we will cover them
    NBDCOMPILE_ASSERT(sizeof(Texture1D) <= sizeof(UINT64) * 2,
                      "BufferByteOffset isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture1DArray) <= sizeof(UINT64) * 2,
                      "BufferByteOffset isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture2D) <= sizeof(UINT64) * 2,
                      "BufferByteOffset isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture2DArray) <= sizeof(UINT64) * 2,
                      "BufferByteOffset isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture3D) <= sizeof(UINT64) * 2,
                      "BufferByteOffset isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture3D) <= sizeof(UINT64) * 2,
                      "BufferByteOffset isn't largest union member!");

    if(desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
    {
      BufferStride = desc.Buffer.StructureByteStride;
      BufferFlags = (uint8_t)desc.Buffer.Flags;

      Buffer.FirstElement = desc.Buffer.FirstElement;
      Buffer.NumElements = desc.Buffer.NumElements;
      Buffer.CounterOffsetInBytes = desc.Buffer.CounterOffsetInBytes;
    }
    else
    {
      BufferStride = desc.BufferByteOffset.StructureByteStride;
      BufferFlags = (uint8_t)desc.Buffer.Flags;

      BufferByteOffset.Offset = desc.BufferByteOffset.Offset;
      BufferByteOffset.Size = desc.BufferByteOffset.Size;
      BufferByteOffset.CounterOffsetInBytes = desc.BufferByteOffset.CounterOffsetInBytes;
    }
  }

  D3D12_UNORDERED_ACCESS_VIEW_DESC AsDesc() const
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};

    desc.Format = (DXGI_FORMAT)Format;
    desc.ViewDimension = (D3D12_UAV_DIMENSION)ViewDimension;

    if(desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
    {
      desc.Buffer.StructureByteStride = BufferStride;
      desc.Buffer.Flags = (D3D12_BUFFER_UAV_FLAGS)BufferFlags;

      desc.Buffer.FirstElement = Buffer.FirstElement;
      desc.Buffer.NumElements = Buffer.NumElements;
      desc.Buffer.CounterOffsetInBytes = Buffer.CounterOffsetInBytes;
    }
    else
    {
      desc.BufferByteOffset.StructureByteStride = BufferStride;
      desc.BufferByteOffset.Flags = (D3D12_BUFFER_UAV_FLAGS)BufferFlags;

      desc.BufferByteOffset.Offset = BufferByteOffset.Offset;
      desc.BufferByteOffset.Size = BufferByteOffset.Size;
      desc.BufferByteOffset.CounterOffsetInBytes = BufferByteOffset.CounterOffsetInBytes;
    }

    return desc;
  }
};

struct D3D12_SHADER_RESOURCE_VIEW_DESC_SQUEEZED
{
  // pull up and compress down to 1 byte the enums that don't have any larger values.
  // note Shader4ComponentMapping only uses the lower 2 bytes - 3 bits per component = 12 bits.
  // Could even be bitpacked with ViewDimension if you wanted to get extreme.
  UINT Shader4ComponentMapping;
  uint8_t Format;
  uint8_t ViewDimension;

  // 2 more bytes here - below union is 8-byte aligned

  union
  {
    D3D12_BUFFER_SRV Buffer;
    D3D12_TEX1D_SRV Texture1D;
    D3D12_TEX1D_ARRAY_SRV Texture1DArray;
    D3D12_TEX2D_SRV Texture2D;
    D3D12_TEX2D_ARRAY_SRV Texture2DArray;
    D3D12_TEX2DMS_SRV Texture2DMS;
    D3D12_TEX2DMS_ARRAY_SRV Texture2DMSArray;
    D3D12_TEX3D_SRV Texture3D;
    D3D12_TEXCUBE_SRV TextureCube;
    D3D12_TEXCUBE_ARRAY_SRV TextureCubeArray;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_SRV AS;
    D3D12_BUFFER_SRV_BYTE_OFFSET BufferByteOffset;
  };

  void Init(const D3D12_SHADER_RESOURCE_VIEW_DESC &desc)
  {
    Format = (uint8_t)desc.Format;
    ViewDimension = (uint8_t)desc.ViewDimension;
    Shader4ComponentMapping = desc.Shader4ComponentMapping;

    // D3D12_TEX2D_ARRAY_SRV should be the largest component, so we can copy it and ensure we've
    // copied the rest.
    NBDCOMPILE_ASSERT(sizeof(Buffer) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(AS) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture1D) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture1DArray) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture2D) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture2DMS) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture2DMSArray) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(Texture3D) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(TextureCube) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(TextureCubeArray) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");
    NBDCOMPILE_ASSERT(sizeof(BufferByteOffset) <= sizeof(Texture2DArray),
                      "Texture2DArray isn't largest union member!");

    Texture2DArray = desc.Texture2DArray;
  }

  D3D12_SHADER_RESOURCE_VIEW_DESC AsDesc() const
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};

    desc.Format = (DXGI_FORMAT)Format;
    desc.ViewDimension = (D3D12_SRV_DIMENSION)ViewDimension;
    desc.Shader4ComponentMapping = Shader4ComponentMapping;

    desc.Texture2DArray = Texture2DArray;

    return desc;
  }
};

enum class D3D12DescriptorType
{
  // we start at 0x1000 since this element will alias with the filter
  // in the sampler, to save space
  Sampler,
  CBV = 0x1000,
  SRV,
  UAV,
  RTV,
  DSV,
  Undefined,
};

DECLARE_REFLECTION_ENUM(D3D12DescriptorType);

struct PortableHandle
{
  PortableHandle() : index(0) {}
  PortableHandle(ResourceId id, uint32_t i) : heap(id), index(i) {}
  PortableHandle(uint32_t i) : index(i) {}
  ResourceId heap;
  uint32_t index;
};

DECLARE_REFLECTION_STRUCT(PortableHandle);

struct D3D12_SAMPLER_DESC_SQUEEZED
{
  // this filter must be first and the same size, since we alias it for the descriptor type
  D3D12_FILTER Filter;

  // we just save the enums in a byte since they'll never be larger
  uint8_t AddressU;
  uint8_t AddressV;
  uint8_t AddressW;
  uint8_t ComparisonFunc;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  // just copy as uint
  FLOAT UintBorderColor[4];
  FLOAT MinLOD;
  FLOAT MaxLOD;
  D3D12_SAMPLER_FLAGS Flags;

  void Init(const D3D12_SAMPLER_DESC2 &desc)
  {
    Filter = desc.Filter;
    AddressU = uint8_t(desc.AddressU);
    AddressV = uint8_t(desc.AddressV);
    AddressW = uint8_t(desc.AddressW);
    ComparisonFunc = uint8_t(desc.ComparisonFunc);
    MipLODBias = desc.MipLODBias;
    MaxAnisotropy = desc.MaxAnisotropy;
    memcpy(UintBorderColor, desc.UintBorderColor, sizeof(UintBorderColor));
    MinLOD = desc.MinLOD;
    MaxLOD = desc.MaxLOD;
    Flags = desc.Flags;
  }

  D3D12_SAMPLER_DESC2 AsDesc() const
  {
    D3D12_SAMPLER_DESC2 desc = {};

    desc.Filter = Filter;
    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE(AddressU);
    desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE(AddressV);
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE(AddressW);
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC(ComparisonFunc);
    desc.MipLODBias = MipLODBias;
    desc.MaxAnisotropy = MaxAnisotropy;
    memcpy(desc.UintBorderColor, UintBorderColor, sizeof(UintBorderColor));
    desc.MinLOD = MinLOD;
    desc.MaxLOD = MaxLOD;
    desc.Flags = Flags;

    return desc;
  }
};

// the heap pointer & index are inside the data structs, because in the sampler case we don't need
// to pad up for any alignment, and in the non-sampler case we declare the type before uint64/ptr
// aligned elements come, so we don't get any padding waste.
struct SamplerDescriptorData
{
  // same location in both structs
  WrappedID3D12DescriptorHeap *heap;
  uint32_t idx;

  D3D12_SAMPLER_DESC_SQUEEZED desc;
};

struct NonSamplerDescriptorData
{
  // same location in both structs
  WrappedID3D12DescriptorHeap *heap;
  uint32_t idx;

  // this element overlaps with the D3D12_FILTER in D3D12_SAMPLER_DESC,
  // with values that are invalid for filter
  D3D12DescriptorType type;

  // we store the ResourceId instead of a pointer here so we can check for invalidation,
  // in case the resource is freed and a different one is allocated in its place.
  // This can happen if e.g. a descriptor is initialised with ResourceId(1234), then the
  // resource is deleted and the descriptor is unused after that, but ResourceId(5678) is
  // allocated with the same ID3D12Resource*. We'd serialise the descriptor pointing to
  // ResourceId(5678) and it may well be completely invalid to create with the other parameters
  // we have stored.
  // We don't need anything but the ResourceId in high-traffic situations
  ResourceId resource;

  // this needs to be out here because we can't have the ResourceId with a constructor in the
  // anonymous union
  ResourceId counterResource;

  union
  {
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv;
    D3D12_SHADER_RESOURCE_VIEW_DESC_SQUEEZED srv;
    D3D12_UNORDERED_ACCESS_VIEW_DESC_SQUEEZED uav;
    D3D12_RENDER_TARGET_VIEW_DESC rtv;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv;
  };
};

union DescriptorData
{
  DescriptorData()
  {
    nonsamp.resource = ResourceId();
    nonsamp.counterResource = ResourceId();
    nonsamp.type = D3D12DescriptorType::Undefined;
  }
  SamplerDescriptorData samp;
  NonSamplerDescriptorData nonsamp;
};

struct D3D12Descriptor
{
public:
  D3D12Descriptor()
  {
    data.samp.heap = NULL;
    data.samp.idx = 0;
  }

  void Setup(WrappedID3D12DescriptorHeap *heap, UINT idx)
  {
    // only need to set this once, it's aliased between samp and nonsamp
    data.samp.heap = heap;
    data.samp.idx = idx;

    // initially descriptors are undefined. This way we just fill them with
    // some null SRV descriptor so it's safe to copy around etc but is no
    // less undefined for the application to use
    data.nonsamp.type = D3D12DescriptorType::Undefined;
  }

  D3D12DescriptorType GetType() const
  {
    NBDCOMPILE_ASSERT(sizeof(D3D12Descriptor) <= 64, "D3D12Descriptor has gotten larger");

    if(data.nonsamp.type < D3D12DescriptorType::CBV)
      return D3D12DescriptorType::Sampler;

    return data.nonsamp.type;
  }

  operator D3D12_CPU_DESCRIPTOR_HANDLE() const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    handle.ptr = (SIZE_T)this;
    return handle;
  }

  operator D3D12_GPU_DESCRIPTOR_HANDLE() const
  {
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    handle.ptr = (SIZE_T)this;
    return handle;
  }

  void Init(const D3D12_SAMPLER_DESC2 *pDesc);
  void Init(const D3D12_SAMPLER_DESC *pDesc);
  void Init(const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc);
  void Init(ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc);
  void Init(ID3D12Resource *pResource, ID3D12Resource *pCounterResource,
            const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc);
  void Init(ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc);
  void Init(ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc);

  void Create(D3D12_DESCRIPTOR_HEAP_TYPE heapType, WrappedID3D12Device *dev,
              D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void CopyFrom(const D3D12Descriptor &src);
  void GetRefIDs(ResourceId &id, ResourceId &id2, FrameRefType &ref);

  WrappedID3D12DescriptorHeap *GetHeap() const { return data.samp.heap; }
  uint32_t GetHeapIndex() const { return data.samp.idx; }
  D3D12_CPU_DESCRIPTOR_HANDLE GetCPU() const;
  D3D12_GPU_DESCRIPTOR_HANDLE GetGPU() const;
  PortableHandle GetPortableHandle() const;

  // descriptor heap itself
  ResourceId GetHeapResourceId() const;
  //
  // a resource - this covers RTV/DSV/SRV resource, UAV main resource (not counter - see below).
  // It does NOT cover the CBV's address - fetch that via GetCBV().BufferLocation
  ResourceId GetResResourceId() const;
  //
  // the counter resource for UAVs
  ResourceId GetCounterResourceId() const;

  // Accessors for descriptor structs. The squeezed structs return only by value, others have const
  // reference returns
  const D3D12_RENDER_TARGET_VIEW_DESC &GetRTV() const { return data.nonsamp.rtv; }
  const D3D12_DEPTH_STENCIL_VIEW_DESC &GetDSV() const { return data.nonsamp.dsv; }
  const D3D12_CONSTANT_BUFFER_VIEW_DESC &GetCBV() const { return data.nonsamp.cbv; }
  // squeezed descriptors
  D3D12_SAMPLER_DESC2 GetSampler() const { return data.samp.desc.AsDesc(); }
  D3D12_UNORDERED_ACCESS_VIEW_DESC GetUAV() const { return data.nonsamp.uav.AsDesc(); }
  D3D12_SHADER_RESOURCE_VIEW_DESC GetSRV() const { return data.nonsamp.srv.AsDesc(); }
private:
  DescriptorData data;

  // allow serialisation function access to the data
  template <class SerialiserType>
  friend void DoSerialise(SerialiserType &ser, D3D12Descriptor &el);
};

DECLARE_REFLECTION_STRUCT(D3D12Descriptor);

inline D3D12Descriptor *GetWrapped(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
  return (D3D12Descriptor *)handle.ptr;
}

inline D3D12Descriptor *GetWrapped(D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
  return (D3D12Descriptor *)handle.ptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE Unwrap(D3D12_CPU_DESCRIPTOR_HANDLE handle);
D3D12_GPU_DESCRIPTOR_HANDLE Unwrap(D3D12_GPU_DESCRIPTOR_HANDLE handle);
D3D12_CPU_DESCRIPTOR_HANDLE UnwrapCPU(D3D12Descriptor *handle);
D3D12_GPU_DESCRIPTOR_HANDLE UnwrapGPU(D3D12Descriptor *handle);

class D3D12ResourceManager;

PortableHandle ToPortableHandle(D3D12Descriptor *handle);
PortableHandle ToPortableHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle);
PortableHandle ToPortableHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle);
D3D12_CPU_DESCRIPTOR_HANDLE CPUHandleFromPortableHandle(D3D12ResourceManager *manager,
                                                        PortableHandle handle);
D3D12_GPU_DESCRIPTOR_HANDLE GPUHandleFromPortableHandle(D3D12ResourceManager *manager,
                                                        PortableHandle handle);
D3D12Descriptor *DescriptorFromPortableHandle(D3D12ResourceManager *manager, PortableHandle handle);

struct DynamicDescriptorWrite
{
  D3D12Descriptor desc;

  D3D12Descriptor *dest;
};

struct DynamicDescriptorCopy
{
  DynamicDescriptorCopy() : dst(NULL), src(NULL), type(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {}
  DynamicDescriptorCopy(D3D12Descriptor *d, D3D12Descriptor *s, D3D12_DESCRIPTOR_HEAP_TYPE t)
      : dst(d), src(s), type(t)
  {
  }

  D3D12Descriptor *dst;
  D3D12Descriptor *src;
  D3D12_DESCRIPTOR_HEAP_TYPE type;
};

DECLARE_REFLECTION_STRUCT(DynamicDescriptorCopy);

struct D3D12ResourceRecord;

struct CmdListRecordingInfo
{
  ChunkPagePool *allocPool = NULL;
  ChunkAllocator *alloc = NULL;

  D3D12ResourceRecord *allocRecord = NULL;

  BarrierSet barriers;

  bool forceMapsListEvent = false;

  // a list of all resources dirtied by this command list
  std::set<ResourceId> dirtied;

  // a list of descriptors that are bound at any point in this command list
  // used to look up all the frame refs per-descriptor and apply them on queue
  // submit with latest binding refs.
  // This stores the start of the range and the number of descriptors, and full
  // traversal occurs during queue submit, to avoid perf issues during regular
  // application operation.
  // We allow duplicates in here since it's a better tradeoff to let the vector
  // expand a bit more to contain duplicates and then deal with it during frame
  // capture, than to constantly be deduplicating during record (e.g. with a
  // set or sorted vector).
  nbdarray<nbdpair<D3D12Descriptor *, UINT>> boundDescs;

  // bundles executed
  nbdarray<D3D12ResourceRecord *> bundles;
};

class WrappedID3D12Resource;
using D3D12BufferOffset = UINT64;

struct MapState
{
  ID3D12Resource *res;
  UINT subres;
  UINT64 totalSize;

  bool operator==(const MapState &o) { return res == o.res && subres == o.subres; }
};

// Enum for the supported heap type for D3D12GpuBuffer allocation
enum class D3D12GpuBufferHeapType
{
  UnInitialized = 0,             // Not initialized
  AccStructDefaultHeap,          // Buffer pool of resource with
                                 // D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
                                 // init state
  ReadBackHeap,                  // Buffer pool with resource on read back heap
  UploadHeap,                    // Buffer Pool with resource on upload heap
  DefaultHeap,                   // Buffer Pool with resource on default heap
  DefaultHeapWithUav,            // Buffer with resource on default heap with UAV enabled
  CustomHeapWithUavCpuAccess,    // Buffer Pool with resource on Custom heap with UAV and CPU access
  Count
};

// Flag for the heap allocation to decide whether to sub-alloc or alloc a dedicated
// heap (currently only implicit heap from CommittedResource is supported)
enum class D3D12GpuBufferHeapMemoryFlag
{
  UnInitialized = 0,
  Default,      // Buffer will be sub-allocated, and heap will be shared with other
  Dedicated,    // Buffer will have a dedicated heap
};

class D3D12GpuBufferAllocator;

struct D3D12GpuBuffer
{
  D3D12GpuBuffer(D3D12GpuBufferAllocator &alloc, D3D12GpuBufferHeapType heapType,
                 D3D12GpuBufferHeapMemoryFlag heapMemory, uint64_t size, uint64_t alignment,
                 D3D12_GPU_VIRTUAL_ADDRESS alignedAddress, ID3D12Resource *resource)
      : m_alignedAddress(alignedAddress),
        m_offset(0),
        m_alignment(alignment),
        m_addressContentSize(size),
        m_heapType(heapType),
        m_heapMemory(heapMemory),
        m_resource(resource),
        m_Allocator(alloc)
  {
    m_RefCount = 1;
    if(m_resource)
    {
      m_offset = alignedAddress - m_resource->GetGPUVirtualAddress();
    }
  }

  // disable copying, these should be passed via pointer so the refcounting works as expected
  D3D12GpuBuffer(const D3D12GpuBuffer &) = delete;
  D3D12GpuBuffer(D3D12GpuBuffer &&) = delete;
  D3D12GpuBuffer &operator=(const D3D12GpuBuffer &) = delete;

  D3D12GpuBufferHeapType HeapType() const { return m_heapType; }
  bool operator!=(const D3D12GpuBuffer &other) const { return !(*this == other); }
  bool operator==(const D3D12GpuBuffer &other) const
  {
    bool equal = true;
    equal &= m_alignedAddress == other.m_alignedAddress;
    equal &= m_alignment == other.m_alignment;
    equal &= m_addressContentSize == other.m_addressContentSize;
    equal &= m_heapType == other.m_heapType;
    equal &= m_heapMemory == other.m_heapMemory;
    equal &= m_resource == other.m_resource;
    equal &= m_offset == other.m_offset;

    return equal;
  }

  ID3D12Resource *Resource() const { return m_resource; };
  uint64_t Offset() const { return m_offset; }
  uint64_t Size() const { return m_addressContentSize; }
  D3D12_GPU_VIRTUAL_ADDRESS Address() const { return m_alignedAddress; }
  uint64_t Alignment() const { return m_alignment; }
  void AddRef();
  void Release();
  D3D12GpuBufferHeapMemoryFlag HeapMemory() const { return m_heapMemory; }

  void *Map(D3D12_RANGE *pReadRange = NULL)
  {
    byte *ret = NULL;
    if(FAILED(m_resource->Map(0, pReadRange, (void **)&ret)))
      return NULL;
    ret += m_offset;
    return ret;
  }
  void Unmap(D3D12_RANGE *pWrittenRange = NULL) { m_resource->Unmap(0, pWrittenRange); }
private:
  unsigned int m_RefCount;
  D3D12_GPU_VIRTUAL_ADDRESS m_alignedAddress;
  uint64_t m_offset;
  uint64_t m_alignment;
  uint64_t m_addressContentSize;
  D3D12GpuBufferAllocator &m_Allocator;
  D3D12GpuBufferHeapType m_heapType;
  D3D12GpuBufferHeapMemoryFlag m_heapMemory;
  ID3D12Resource *m_resource;
};

struct D3D12ResourceRecord : public ResourceRecord
{
  enum
  {
    NullResource = NULL
  };

  D3D12ResourceRecord(ResourceId id)
      : ResourceRecord(id, true),
        type(Resource_Unknown),
        ContainsExecuteIndirect(false),
        cmdInfo(NULL),
        sparseTable(NULL),
        m_Maps(NULL),
        m_MapsCount(0),
        bakedCommands(NULL)
  {
  }
  ~D3D12ResourceRecord()
  {
    if(type == Resource_CommandAllocator)
    {
      SAFE_DELETE(cmdInfo->alloc);
      SAFE_DELETE(cmdInfo->allocPool);
    }
    SAFE_DELETE(cmdInfo);
    SAFE_DELETE(sparseTable);
    SAFE_DELETE_ARRAY(m_Maps);
  }
  void Bake()
  {
    NBDASSERT(cmdInfo);
    SwapChunks(bakedCommands);
    cmdInfo->barriers.swap(bakedCommands->cmdInfo->barriers);
    cmdInfo->dirtied.swap(bakedCommands->cmdInfo->dirtied);
    cmdInfo->boundDescs.swap(bakedCommands->cmdInfo->boundDescs);
    cmdInfo->bundles.swap(bakedCommands->cmdInfo->bundles);
    bakedCommands->cmdInfo->forceMapsListEvent = cmdInfo->forceMapsListEvent;
    bakedCommands->cmdInfo->alloc = cmdInfo->alloc;
    bakedCommands->cmdInfo->allocRecord = cmdInfo->allocRecord;
  }

  D3D12ResourceType type;
  bool ContainsExecuteIndirect;
  D3D12ResourceRecord *bakedCommands;
  CmdListRecordingInfo *cmdInfo;
  Sparse::PageTable *sparseTable;

  struct MapData
  {
    MapData() : refcount(0), realPtr(NULL), shadowPtr(NULL) {}
    int32_t refcount;
    byte *realPtr;
    byte *shadowPtr;
  };

  MapData *m_Maps;
  size_t m_MapsCount;
  Threading::CriticalSection m_MapLock;
};

struct SparseBinds
{
  SparseBinds(const Sparse::PageTable &table);
  // tagged constructor meaning 'null binds everywhere'
  SparseBinds(int);

  void Apply(WrappedID3D12Device *device, ID3D12Resource *resource);

private:
  bool null = false;

  struct Bind
  {
    ResourceId heap;
    D3D12_TILED_RESOURCE_COORDINATE regionStart;
    D3D12_TILE_REGION_SIZE regionSize;
    D3D12_TILE_RANGE_FLAGS rangeFlag;
    UINT rangeOffset;
    UINT rangeCount;
  };
  nbdarray<Bind> binds;
};

struct ASBuildData;

struct D3D12InitialContents
{
  enum Tag
  {
    Copy,
    // this is only valid during capture - it indicates we didn't create a staging texture, we're
    // going to read directly from the resource (only valid for resources that are already READBACK)
    MapDirect,
    // for created initial states we always have an identical resource
    ForceCopy,
    // for handling acceleration structures
    AccelerationStructure,
    // for sparse buffers with no contents
    SparseOnly,
  };
  D3D12InitialContents(D3D12Descriptor *d, uint32_t n) : D3D12InitialContents()
  {
    tag = Copy;
    resourceType = Resource_DescriptorHeap;
    descriptors = d;
    numDescriptors = n;
  }
  D3D12InitialContents(ID3D12DescriptorHeap *r) : D3D12InitialContents()
  {
    tag = Copy;
    resourceType = Resource_DescriptorHeap;
    resource = r;
  }
  D3D12InitialContents(ID3D12Resource *r) : D3D12InitialContents()
  {
    tag = Copy;
    resourceType = Resource_Resource;
    resource = r;
  }
  D3D12InitialContents(byte *data, size_t size) : D3D12InitialContents()
  {
    tag = MapDirect;
    resourceType = Resource_Resource;
    srcData = data;
    dataSize = size;
  }
  D3D12InitialContents(Tag tg, D3D12ResourceType type) : D3D12InitialContents()
  {
    tag = tg;
    resourceType = type;
  }
  D3D12InitialContents(Tag tg, ID3D12Resource *r) : D3D12InitialContents()
  {
    tag = tg;
    if(r)
    {
      r->AddRef();
    }
    resourceType = Resource_Resource;
    resource = r;
  }
  D3D12InitialContents()
      : tag(Copy),
        resourceType(Resource_Unknown),
        descriptors(NULL),
        numDescriptors(0),
        resource(NULL),
        srcData(NULL),
        dataSize(0),
        sparseTable(NULL),
        sparseBinds(NULL),
        buildData(NULL),
        cachedBuiltAS(NULL)
  {
  }

  template <typename Configuration>
  void Free(ResourceManager<Configuration> *rm)
  {
    SAFE_DELETE_ARRAY(descriptors);
    SAFE_DELETE(sparseTable);
    SAFE_RELEASE(resource);
    FreeAlignedBuffer(srcData);
    SAFE_RELEASE(buildData);
    SAFE_RELEASE(cachedBuiltAS);
  }

  Tag tag;
  D3D12ResourceType resourceType;
  D3D12Descriptor *descriptors;
  uint32_t numDescriptors;
  ID3D12DeviceChild *resource;
  byte *srcData;
  size_t dataSize;
  nbdarray<nbdstr> descriptorNames;

  nbdarray<uint32_t> subresources;

  // only valid on capture - the snapshotted table at prepare time
  Sparse::PageTable *sparseTable;
  // only valid on replay, the table above converted into a set of binds
  SparseBinds *sparseBinds;

  ASBuildData *buildData;
  // only on replay, we cache the result of the build so we can copy it instead to save time
  D3D12GpuBuffer *cachedBuiltAS;
};

class WrappedID3D12GraphicsCommandList;

// class for allocating GPU Buffer
class D3D12GpuBufferAllocator
{
public:
  D3D12GpuBufferAllocator(WrappedID3D12Device *wrappedDevice) : m_wrappedDevice(wrappedDevice)
  {
    m_totalAllocatedMemoryInUse = 0;
  }

  bool Alloc(D3D12GpuBufferHeapType heapType, D3D12GpuBufferHeapMemoryFlag heapMem, uint64_t size,
             D3D12GpuBuffer **gpuBuffer)
  {
    return Alloc(heapType, heapMem, size, 0, gpuBuffer);
  }

  bool Alloc(D3D12GpuBufferHeapType heapType, D3D12GpuBufferHeapMemoryFlag heapMem, uint64_t size,
             uint64_t alignment, D3D12GpuBuffer **gpuBuffer);

  void Release(const D3D12GpuBuffer &gpuBuffer);

  uint64_t GetAllocatedMemorySize() const { return m_totalAllocatedMemoryInUse; }
  ~D3D12GpuBufferAllocator()
  {
    for(D3D12GpuBufferPool *bufferPool : m_bufferPoolList)
    {
      SAFE_DELETE(bufferPool);
    }
  }

private:
  // Class for handling buffer resources
  class D3D12GpuBufferResource
  {
  public:
    static bool CreateBufferResource(WrappedID3D12Device *wrappedDevice,
                                     D3D12GpuBufferHeapType heapType, uint64_t size,
                                     D3D12GpuBufferResource **bufferResource);
    static bool CreateCommittedResourceBuffer(ID3D12Device *device,
                                              const D3D12_HEAP_PROPERTIES &heapProperty,
                                              D3D12_RESOURCE_STATES initState, uint64_t size,
                                              bool allowUav, D3D12GpuBufferResource **bufferResource);
    static bool ReleaseGpuBufferResource(D3D12GpuBufferResource *bufferResource);

    D3D12GpuBufferResource() = delete;

    ID3D12Resource *Resource() const { return m_resource; }
    ~D3D12GpuBufferResource() { SAFE_RELEASE(m_resource); }
    D3D12GpuBufferResource(ID3D12Resource *resource, D3D12_HEAP_TYPE heapType);

    bool SubAllocationInRange(D3D12_GPU_VIRTUAL_ADDRESS gpuAddress) const
    {
      return (m_resourceGpuAddressRange.start <= gpuAddress &&
              gpuAddress < m_resourceGpuAddressRange.realEnd);
    }

    bool Free(D3D12_GPU_VIRTUAL_ADDRESS gpuAddress, uint64_t size, uint64_t alignment)
    {
      uint64_t offset = gpuAddress - m_resourceGpuAddressRange.start;
      auto iter = m_subRanges.find(offset);
      if(iter != m_subRanges.end() && iter->value() == D3D12SubRangeFlag::Used)
      {
        uint64_t iterOffset = iter->start();
        uint64_t alignedOffset = iterOffset;
        if(alignment)
          alignedOffset = AlignUp(m_resourceGpuAddressRange.start + alignedOffset, alignment) -
                          m_resourceGpuAddressRange.start;

        uint64_t padding = alignedOffset - iterOffset;

        m_bytesFree += size + padding;
        iter->setValue(D3D12SubRangeFlag::Free);
        // Merging will only occur if the adjacent sub-ranges are also free
        iter->mergeLeft();
        m_lastFree = iter;

        ++iter;
        if(iter != m_subRanges.end())
        {
          iter->mergeLeft();
          m_lastFree = iter;
        }

        return true;
      }
      return false;
    }

    bool SubAlloc(uint64_t size, uint64_t alignment, D3D12_GPU_VIRTUAL_ADDRESS &address)
    {
      uint64_t resourceWidth = m_resourceGpuAddressRange.realEnd - m_resourceGpuAddressRange.start;

      for(auto iter = m_lastFree; iter != m_subRanges.end(); ++iter)
      {
        if(iter->value() == D3D12SubRangeFlag::Free)
        {
          uint64_t freeRangeStart = iter->start();
          uint64_t freeRangeEnd = NBDMIN(iter->finish(), resourceWidth);
          uint64_t alignedStart = freeRangeStart;

          if(alignment)
            alignedStart = AlignUp(m_resourceGpuAddressRange.start + alignedStart, alignment) -
                           m_resourceGpuAddressRange.start;

          uint64_t padding = alignedStart - freeRangeStart;

          if(alignedStart < freeRangeEnd && alignedStart + size <= freeRangeEnd)
          {
            iter->setValue(D3D12SubRangeFlag::Used);
            address = m_resourceGpuAddressRange.start + alignedStart;
            // Split the sub-range if there's extra space beyond this allocation
            if(alignedStart + size < freeRangeEnd)
            {
              iter->split(alignedStart + size);
              iter->setValue(D3D12SubRangeFlag::Free);
            }

            m_bytesFree -= size + padding;

            m_lastFree = iter;

            return true;
          }
        }
      }
      return false;
    }

    enum class D3D12SubRangeFlag
    {
      Free = 0,
      Used
    };

    Intervals<D3D12SubRangeFlag> m_subRanges;
    Intervals<D3D12SubRangeFlag>::iterator m_lastFree;
    GPUAddressRange m_resourceGpuAddressRange;
    ID3D12Resource *m_resource;
    D3D12_RESOURCE_DESC m_resDesc;
    D3D12_HEAP_TYPE m_heapType;
    uint64_t m_bytesFree;
  };

  class D3D12GpuBufferPool
  {
  public:
    D3D12GpuBufferPool(D3D12GpuBufferHeapType bufferPoolType, uint64_t bufferInitialSize)
        : m_bufferPoolHeapType(bufferPoolType), m_bufferInitSize(bufferInitialSize)
    {
    }

    ~D3D12GpuBufferPool()
    {
      for(D3D12GpuBufferResource *bufferRes : m_bufferResourceList)
      {
        SAFE_DELETE(bufferRes);
      }
    }

    bool Alloc(WrappedID3D12Device *wrappedDevice, D3D12GpuBufferHeapMemoryFlag heapMem,
               uint64_t size, uint64_t alignment, D3D12GpuBuffer **gpuBuffer);

    void Free(const D3D12GpuBuffer &gpuBuffer);

    static constexpr uint64_t kDefaultWithUavSizeBufferInitSize = 1000ull * 8u;
    static constexpr uint64_t kAccStructBufferPoolInitSize = 1000ull * 256u;

  private:
    nbdarray<D3D12GpuBufferResource *> m_bufferResourceList;
    D3D12GpuBufferHeapType m_bufferPoolHeapType;
    uint64_t m_bufferInitSize;
  };

  Threading::CriticalSection m_bufferAllocLock;
  D3D12GpuBufferPool *m_bufferPoolList[(size_t)D3D12GpuBufferHeapType::Count] = {};

  WrappedID3D12Device *m_wrappedDevice;
  // keeps track of the allocated memory in use,
  // and not the actual amount of memory allocated
  uint64_t m_totalAllocatedMemoryInUse;
};

enum class D3D12PatchTLASBuildParam
{
  RootConstantBuffer,
  RootAddressPairSrv,
  RootPatchedAddressUav,
  Count
};

enum class D3D12TLASInstanceCopyParam
{
  RootCB,
  SourceSRV,
  DestUAV,
  RootAddressPairSrv,
  Count
};

enum class D3D12IndirectPrepParam
{
  GeneralCB,
  AppExecuteArgs,
  AppCount,
  PatchedExecuteArgs,
  InternalExecuteArgs,
  InternalExecuteCount,
  Count,
};

enum class D3D12PatchRayDispatchParam
{
  GeneralCB,
  RecordCB,
  SourceBuffer,
  DestBuffer,
  StateObjectData,
  RecordData,
  RootSigData,
  AddrPatchData,
  Count,
};

struct D3D12AccStructPatchInfo
{
  ID3D12RootSignature *m_rootSignature = NULL;
  ID3D12PipelineState *m_pipeline = NULL;
};

class WrappedID3D12CommandSignature;

struct PatchedRayDispatch
{
  struct Resources
  {
    // the lookup buffer
    D3D12GpuBuffer *lookupBuffer;
    // the scratch buffer used for patching's fence.
    D3D12GpuBuffer *patchScratchBuffer;
    // the argument buffer used for indirect executes.
    D3D12GpuBuffer *argumentBuffer;

    D3D12GpuBuffer *readbackBuffer;

    uint32_t query;

    // for convenience, when these resources are referenced in a queue they get a fence value to
    // indicate when they're safe to release. This values are unset when returned from patching or
    // referenced in the list and is set in each queue's copy of the references.
    UINT64 fenceValue = 0;

    void AddRef() const
    {
      SAFE_ADDREF(lookupBuffer);
      SAFE_ADDREF(patchScratchBuffer);
      SAFE_ADDREF(argumentBuffer);
      SAFE_ADDREF(readbackBuffer);
    }

    void Release()
    {
      SAFE_RELEASE(lookupBuffer);
      SAFE_RELEASE(patchScratchBuffer);
      SAFE_RELEASE(argumentBuffer);
      SAFE_RELEASE(readbackBuffer);
    }
  };

  Resources resources;

  // the patched dispatch descriptor
  D3D12_DISPATCH_RAYS_DESC desc = {};
  nbdarray<ResourceId> heaps;
  // for auditing, from an indirect RT dispatch
  UINT MaxCommands = 0;
  WrappedID3D12CommandSignature *comSig = NULL;
  bool HasDynamicCount = false;
};

struct D3D12ShaderExportDatabase;

struct ASStats
{
  struct
  {
    uint32_t msThreshold;
    uint32_t count;
    uint64_t bytes;
  } bucket[4];

  uint64_t overheadBytes;
  uint64_t diskBytes;
  uint32_t diskCached;
};

struct RTGPUPatchingStats
{
  uint32_t builds;
  uint64_t buildBytes;
  double totalBuildMS;

  uint32_t dispatches;
  double totalDispatchesMS;
};

struct DiskCachedAS
{
  size_t fileIndex = ~0U;
  uint64_t offset = 0;
  uint64_t size = 0;

  bool Valid() const { return fileIndex != ~0U; }
};

// this is a refcounted GPU buffer with the build data, together with the metadata
struct ASBuildData
{
  static const uint64_t NULLVA = ~0ULL;
  // RVA equivalent of D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE
  struct RVAWithStride
  {
    uint64_t RVA;
    UINT64 StrideInBytes;
  };

  // RVA equivalent of D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC
  struct RVATrianglesDesc
  {
    uint64_t Transform3x4;
    DXGI_FORMAT IndexFormat;
    DXGI_FORMAT VertexFormat;
    UINT IndexCount;
    UINT VertexCount;
    uint64_t IndexBuffer;
    RVAWithStride VertexBuffer;
  };

  // RVA equivalent of D3D12_RAYTRACING_GEOMETRY_AABBS_DESC
  struct RVAAABBDesc
  {
    UINT AABBCount;
    RVAWithStride AABBs;
  };

  // analogous struct to D3D12_RAYTRACING_GEOMETRY_DESC but contains plain uint64 offsets in place
  // of GPU VAs - effectively RVAs in the internal buffer
  struct RTGeometryDesc
  {
    RTGeometryDesc() = default;
    RTGeometryDesc(const D3D12_RAYTRACING_GEOMETRY_DESC &desc)
    {
      NBDCOMPILE_ASSERT(sizeof(*this) == sizeof(D3D12_RAYTRACING_GEOMETRY_DESC),
                        "Types should be entirely identical");
      memcpy(this, &desc, sizeof(desc));
    }

    D3D12_RAYTRACING_GEOMETRY_TYPE Type;
    D3D12_RAYTRACING_GEOMETRY_FLAGS Flags;

    union
    {
      RVATrianglesDesc Triangles;
      RVAAABBDesc AABBs;
    };
  };

  // this struct is immutable, it's a snapshot of data and it's only referenced or deleted, never modified
  ASBuildData(const ASBuildData &o) = delete;
  ASBuildData(ASBuildData &&o) = delete;
  ASBuildData &operator=(const ASBuildData &o) = delete;

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE Type;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags;

  // for TLAS, the number of instance descriptors. For BLAS the number of geometries is given by
  // the size of the array below
  UINT NumBLAS;

  // geometry GPU addresses have been de-based to contain only offsets
  nbdarray<RTGeometryDesc> geoms;

  void MarkWorkComplete();
  bool IsWorkComplete() const { return complete; }

  void AddRef();
  void Release();

  D3D12GpuBuffer *buffer = NULL;
  DiskCachedAS diskCache;
  uint32_t query = 0;

  std::function<bool()> cleanupCallback;

private:
  ASBuildData() = default;

  friend class D3D12RTManager;
  friend class D3D12ResourceManager;

  D3D12RTManager *rtManager = NULL;

  // timestamp this build data was recorded on
  double timestamp = 0;

  // has the GPU work for this build data finished and synchronised?
  bool complete = false;

  // how many bytes of overhead are currently present, due to copying with strided vertex/AABB data
  uint64_t bytesOverhead = 0;

  unsigned int m_RefCount = 1;
};

DECLARE_REFLECTION_STRUCT(ASBuildData::RVAWithStride);
DECLARE_REFLECTION_STRUCT(ASBuildData::RVATrianglesDesc);
DECLARE_REFLECTION_STRUCT(ASBuildData::RVAAABBDesc);
DECLARE_REFLECTION_STRUCT(ASBuildData::RTGeometryDesc);

class D3D12RTManager
{
public:
  D3D12RTManager(WrappedID3D12Device *device, D3D12GpuBufferAllocator &gpuBufferAllocator);
  ~D3D12RTManager();

  void InitInternalResources();

  D3D12AccStructPatchInfo GetAccStructPatchInfo() const { return m_accStructPatchInfo; }

  uint32_t RegisterLocalRootSig(const D3D12RootSignature &sig);

  void RegisterExportDatabase(D3D12ShaderExportDatabase *db);
  void UnregisterExportDatabase(D3D12ShaderExportDatabase *db);

  void PrepareRayDispatchBuffer(GPUAddressRangeTracker *origAddresses);

  ASBuildData *CopyBuildInputs(ID3D12GraphicsCommandList4 *unwrappedCmd,
                               const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs);
  void RemoveASBuildData(ASBuildData *data)
  {
    SCOPED_LOCK(m_ASBuildDataLock);
    if(data->buffer)
      m_InMemASBuildDatas.removeOne(data);
    else
      m_DiskCachedASBuildDatas.removeOne(data);
  }

  void GatherRTStatistics(ASStats &blasAges, ASStats &tlasAges, RTGPUPatchingStats &gpuStats);

  D3D12GpuBuffer *UnrollBLASInstancesList(
      ID3D12GraphicsCommandList4 *unwrappedCmd,
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs,
      D3D12_GPU_VIRTUAL_ADDRESS addressPairResAddress, uint64_t addressCount,
      D3D12GpuBuffer *copyDestUAV);

  PatchedRayDispatch PatchRayDispatch(ID3D12GraphicsCommandList4 *unwrappedCmd,
                                      nbdarray<ResourceId> heaps,
                                      const D3D12_DISPATCH_RAYS_DESC &desc);
  PatchedRayDispatch PatchIndirectRayDispatch(ID3D12GraphicsCommandList *unwrappedCmd,
                                              nbdarray<ResourceId> heaps,
                                              ID3D12CommandSignature *pCommandSignature,
                                              UINT &MaxCommandCount, ID3D12Resource *pArgumentBuffer,
                                              UINT64 ArgumentBufferOffset,
                                              ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset);

  void AddPendingCallbacks(ID3D12Fence *fence, UINT64 waitValue,
                           const nbdarray<std::function<bool()>> &callbacks);
  void TickASManagement();

  // this disk cache is primarily single threaded - either the disk cache thread owns
  // seeking/writing to the files, or during initial states that thread owns seeking/reading.
  // we lock around this access only for allocating from blocks
  struct DiskCacheFile
  {
    FILE *file = NULL;

    // each block is 1kB to split the difference between caching lots of tiny ASs and wasting space,
    // vs tracking many blocks
    static const uint64_t blockSize = 1 * 1024;
    static const uint64_t blocksInFile = 64 * 1024;

    // one per block
    bool blocksUsed[blocksInFile] = {};
  };
  Threading::CriticalSection m_DiskCacheLock;
  nbdarray<DiskCacheFile> m_DiskCache;

  DiskCachedAS AllocDiskCache(uint64_t byteSize);
  void FillDiskCache(DiskCachedAS diskCache, void *data);
  void ReleaseDiskCache(DiskCachedAS diskCache);
  template <typename SerialiserType>
  void ReadDiskCache(SerialiserType &ser, nbdliteral name, DiskCachedAS diskCache)
  {
    if(!diskCache.Valid())
      return;

    // this lock should have no contention, we should only be doing this during initial state
    // serialisation when nothing is allocating and the disk cache thread has been flushed
    SCOPED_LOCK(m_DiskCacheLock);

    if(diskCache.fileIndex >= m_DiskCache.size())
    {
      NBDERR("Invalid disk cache file %zu vs %zu", diskCache.fileIndex, m_DiskCache.size());
      return;
    }

    FILE *f = m_DiskCache[diskCache.fileIndex].file;

    FileIO::fseek64(f, diskCache.offset, SEEK_SET);

    {
      StreamReader reader(f, diskCache.size, Ownership::Nothing);
      ser.SerialiseStream(name, reader);
    }
  }

  void PushDiskCacheTask(std::function<void()> task);
  void FlushDiskCacheThread();

  void ResizeSerialisationBuffer(UINT64 ScratchDataSizeInBytes);

  // buffer in UAV state for emitting AS queries to, CPU accessible/mappable
  D3D12GpuBuffer *ASQueryBuffer = NULL;

  // temp buffer for AS serialise copies
  D3D12GpuBuffer *ASSerialiseBuffer = NULL;

  // readback buffer during auditing for evaluating postbuild information
  D3D12GpuBuffer *PostbuildReadbackBuffer = NULL;

  double GetCurrentASTimestamp() { return m_Timestamp.GetMilliseconds(); }

  void Verify(PatchedRayDispatch &r);

  void VerifyDispatch(D3D12_DISPATCH_RAYS_DESC desc, byte *wrappedRecords, byte *unwrappedRecords,
                      WrappedID3D12DescriptorHeap *resHeap, WrappedID3D12DescriptorHeap *sampHeap);
  void VerifyRecord(const uint64_t recordSize, byte *wrappedRecord, byte *unwrappedRef,
                    WrappedID3D12DescriptorHeap *resHeap, WrappedID3D12DescriptorHeap *sampHeap);

  void AddDispatchTimer(uint32_t q);
  void AddBuildTimer(uint32_t q, uint64_t size);

private:
  void InitRayDispatchPatchingResources();
  void InitTLASInstanceCopyingResources();
  void InitReplayBlasPatchingResources();

  void CheckASCaching();
  void CheckPendingCallbacks();

  void CopyFromVA(ID3D12GraphicsCommandList4 *unwrappedCmd, ID3D12Resource *dstRes,
                  uint64_t dstOffset, D3D12_GPU_VIRTUAL_ADDRESS sourceVA, uint64_t byteSize);

  WrappedID3D12Device *m_wrappedDevice;
  D3D12GpuBufferAllocator &m_GPUBufferAllocator;

  PerformanceTimer m_Timestamp;

  D3D12AccStructPatchInfo m_accStructPatchInfo;

  Threading::CriticalSection m_LookupBufferLock;

  D3D12GpuBuffer *m_LookupBuffer = NULL;
  D3D12_GPU_VIRTUAL_ADDRESS m_LookupAddrs[4] = {};
  uint32_t m_NumPatchingAddrs = 0;

  // each unique set of descriptor table offsets are stored here, so any root signatures which
  // only vary in ways that don't affect which tables are contained within them (and so don't
  // need patching) will have a single entry in here
  nbdarray<nbdarray<uint32_t>> m_UniqueLocalRootSigs;

  // export databases that are alive
  nbdarray<D3D12ShaderExportDatabase *> m_ExportDatabases;

  Threading::CriticalSection m_ASBuildDataLock;
  nbdarray<ASBuildData *> m_InMemASBuildDatas;
  nbdarray<ASBuildData *> m_DiskCachedASBuildDatas;

  // is the lookup buffer dirty and needs to be recreated with the latest data?
  bool m_LookupBufferDirty = true;

  // pipeline data for indirect-copying instances in a TLAS build
  struct
  {
    D3D12GpuBuffer *ArgsBuffer = NULL;
    D3D12GpuBuffer *ScratchBuffer = NULL;
    ID3D12PipelineState *PreparePipe = NULL;
    ID3D12PipelineState *CopyPipe = NULL;
    ID3D12RootSignature *RootSig = NULL;
    ID3D12CommandSignature *IndirectSig = NULL;
  } m_TLASCopyingData;

  // pipeline data for patching ray dispatches
  struct
  {
    ID3D12RootSignature *shaderTablePatchRootSig = NULL;
    ID3D12PipelineState *shaderTablePatchPipe = NULL;
    ID3D12PipelineState *shaderTableCopyPipe = NULL;
    ID3D12RootSignature *indirectPrepRootSig = NULL;
    ID3D12PipelineState *indirectPrepPipe = NULL;
    ID3D12CommandSignature *indirectComSig = NULL;
  } m_RayPatchingData;

  Threading::CriticalSection m_ASCacheThreadLock;
  int32_t m_ASCacheThreadRunning = 0;
  int32_t m_ASCacheThreadActive = 0;
  Threading::Semaphore *m_ASCacheThreadSemaphore = NULL;
  Threading::ThreadHandle m_ASCacheThread = {};
  nbdarray<std::function<void()>> m_ASCacheTasks;

  ID3D12QueryHeap *m_TimerQueryHeap = NULL;
  D3D12GpuBuffer *m_TimerReadbackBuffer = NULL;
  uint64_t *m_Timestamps = NULL;
  uint64_t m_TimerFrequency;
  Threading::CriticalSection m_TimerStatsLock;
  nbdarray<uint32_t> m_FreeQueries;
  RTGPUPatchingStats m_AccumulatedStats = {};

  uint32_t GetFreeQuery();

  struct PendingCallbacks
  {
    ID3D12Fence *fence;
    UINT64 fenceValue;
    std::function<bool()> callback;
  };
  Threading::CriticalSection m_PendingCallbacksLock;
  nbdarray<PendingCallbacks> m_PendingCallbacks;
};

struct D3D12ResourceManagerConfiguration
{
  typedef ID3D12DeviceChild *WrappedResourceType;
  typedef ID3D12DeviceChild *RealResourceType;
  typedef D3D12ResourceRecord RecordType;
  typedef D3D12InitialContents InitialContentData;
};

class D3D12ResourceManager : public ResourceManager<D3D12ResourceManagerConfiguration>
{
public:
  D3D12ResourceManager(CaptureState &state, WrappedID3D12Device *dev)
      : ResourceManager(state), m_Device(dev), m_GPUBufferAllocator(dev)
  {
    m_RTManager = new D3D12RTManager(m_Device, m_GPUBufferAllocator);
  }

  ~D3D12ResourceManager() { SAFE_DELETE(m_RTManager); }

  template <class T>
  T *GetResAs(ResourceId id, bool optional = false)
  {
    return (T *)GetResource(id, optional);
  }

  template <typename D3D12Type>
  D3D12Type *CreateDeferredHandle()
  {
    D3D12Type *ret = (D3D12Type *)(m_DummyHandle);

    Atomic::Dec64((int64_t *)&m_DummyHandle);

    return ret;
  }

  void ResolveDeferredWrappers();

  void ApplyBarriers(BarrierSet &barriers, std::map<ResourceId, SubresourceStateVector> &states);

  D3D12RTManager *GetRTManager() const { return m_RTManager; }

  D3D12GpuBufferAllocator &GetGPUBufferAllocator() { return m_GPUBufferAllocator; }

  void AddPlacedResource(ResourceId resId, ResourceId heapId)
  {
    SCOPED_LOCK(m_PlacedLock);
    m_PlacedHeapForResource[resId] = heapId;
  }

  void RemovePlacedResource(ResourceId resId)
  {
    SCOPED_LOCK(m_PlacedLock);
    m_PlacedHeapForResource.erase(resId);
  }

  ResourceId GetPlacedHeapForResource(ResourceId resId)
  {
    SCOPED_LOCK(m_PlacedLock);
    auto it = m_PlacedHeapForResource.find(resId);
    if(it == m_PlacedHeapForResource.end())
      return ResourceId();
    return it->second;
  }

  template <typename Compose>
  void MarkResourceFrameReferenced(ResourceId id, FrameRefType refType, Compose comp)
  {
    ResourceManager::MarkResourceFrameReferenced(id, refType, comp);
    ResourceId id2 = GetPlacedHeapForResource(id);
    if(id2 != ResourceId())
      ResourceManager::MarkResourceFrameReferenced(id2, refType, comp);
  }

  inline void MarkResourceFrameReferenced(ResourceId id, FrameRefType refType)
  {
    ResourceManager::MarkResourceFrameReferenced(id, refType);
    ResourceId id2 = GetPlacedHeapForResource(id);
    if(id2 != ResourceId())
      ResourceManager::MarkResourceFrameReferenced(id2, refType);
  }

  template <typename SerialiserType>
  void SerialiseResourceStates(SerialiserType &ser, BarrierSet &barriers,
                               std::map<ResourceId, SubresourceStateVector> &states,
                               const std::map<ResourceId, SubresourceStateVector> &initialStates);

  template <typename SerialiserType>
  bool Serialise_InitialState(SerialiserType &ser, ResourceId id, D3D12ResourceRecord *record,
                              const D3D12InitialContents *initial);

  void SetInternalResource(ID3D12DeviceChild *res);

private:
  ResourceId GetID(ID3D12DeviceChild *res);

  bool ResourceTypeRelease(ID3D12DeviceChild *res);

  bool Prepare_InitialState(ID3D12DeviceChild *res);
  uint64_t GetSize_InitialState(ResourceId id, const D3D12InitialContents &data);
  bool Serialise_InitialState(WriteSerialiser &ser, ResourceId id, D3D12ResourceRecord *record,
                              const D3D12InitialContents *initial)
  {
    return Serialise_InitialState<WriteSerialiser>(ser, id, record, initial);
  }
  void Create_InitialState(ResourceId id, ID3D12DeviceChild *live, bool hasData);
  void Apply_InitialState(ID3D12DeviceChild *live, D3D12InitialContents &data);
  nbdarray<ResourceId> InitialContentResources();

  WrappedID3D12Device *m_Device;
  D3D12RTManager *m_RTManager;
  D3D12GpuBufferAllocator m_GPUBufferAllocator;

  Threading::CriticalSection m_PlacedLock;
  std::unordered_map<ResourceId, ResourceId> m_PlacedHeapForResource;

  // dummy handle to use - starting from near highest valid pointer to minimise risk of overlap with real handles
  static const uint64_t FirstDummyHandle = UINTPTR_MAX - 1024;
  uint64_t m_DummyHandle = FirstDummyHandle;
};
