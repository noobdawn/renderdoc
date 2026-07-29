/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
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

#include "d3d12_device.h"
#include "driver/dxgi/dxgi_common.h"
#include "d3d12_resources.h"

// pass these through unmodified
HRESULT STDMETHODCALLTYPE
WrappedID3D12Device::RegisterTrimNotificationCallback(D3D12_REGISTER_TRIM_NOTIFICATION *pData)
{
  return m_pDevice15->RegisterTrimNotificationCallback(pData);
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::UnregisterTrimNotificationCallback(DWORD CallbackCookie)
{
  return m_pDevice15->UnregisterTrimNotificationCallback(CallbackCookie);
}

// implementations of TryCreate* for descriptors are with their associated non-try functions to share implementations

template <typename SerialiserType>
bool WrappedID3D12Device::Serialise_CreateQueryHeap1(SerialiserType &ser,
                                                     const D3D12_QUERY_HEAP_DESC *pDesc,
                                                     D3D12_QUERY_HEAP_FLAGS Flags, REFIID riid,
                                                     void **ppvHeap)
{
  SERIALISE_ELEMENT_LOCAL(Descriptor, *pDesc).Named("pDesc"_lit).Important();
  SERIALISE_ELEMENT(Flags);
  SERIALISE_ELEMENT_LOCAL(guid, riid).Named("riid"_lit);
  SERIALISE_ELEMENT_LOCAL(pQueryHeap, ((WrappedID3D12QueryHeap *)*ppvHeap)->GetResourceID())
      .TypedAs("ID3D12QueryHeap *"_lit);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(!m_pDevice15)
    {
      SET_ERROR_RESULT(m_FailedReplayResult, ResultCode::APIHardwareUnsupported,
                       "Capture requires ID3D12Device15 which isn't available");
      return false;
    }

    ID3D12QueryHeap *ret = NULL;
    HRESULT hr = m_pDevice15->CreateQueryHeap1(&Descriptor, Flags, guid, (void **)&ret);

    if(FAILED(hr))
    {
      SET_ERROR_RESULT(m_FailedReplayResult, ResultCode::APIReplayFailed,
                       "Failed creating query heap, HRESULT: %s", ToStr(hr).c_str());
      return false;
    }
    else
    {
      ret = new WrappedID3D12QueryHeap(pQueryHeap, ret, Descriptor, this);
    }

    AddResource(pQueryHeap, ResourceType::Query, "Query Heap");
  }

  return true;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateQueryHeap1(const D3D12_QUERY_HEAP_DESC *pDesc,
                                                                D3D12_QUERY_HEAP_FLAGS Flags,
                                                                REFIID riid, void **ppvHeap)
{
  if(ppvHeap == NULL)
    return m_pDevice15->CreateQueryHeap1(pDesc, Flags, riid, NULL);

  if(riid != __uuidof(ID3D12QueryHeap))
    return E_NOINTERFACE;

  ID3D12QueryHeap *real = NULL;
  HRESULT ret;
  SERIALISE_TIME_CALL(ret = m_pDevice15->CreateQueryHeap1(pDesc, Flags, riid, (void **)&real));

  if(SUCCEEDED(ret))
  {
    WrappedID3D12QueryHeap *wrapped = new WrappedID3D12QueryHeap(ResourceId(), real, *pDesc, this);

    if(IsCaptureMode(m_State))
    {
      CACHE_THREAD_SERIALISER();

      SCOPED_SERIALISE_CHUNK(D3D12Chunk::Device_CreateQueryHeap1);
      Serialise_CreateQueryHeap1(ser, pDesc, Flags, riid, (void **)&wrapped);

      D3D12ResourceRecord *record = GetResourceManager()->AddResourceRecord(wrapped->GetResourceID());
      record->type = Resource_QueryHeap;
      record->Length = 0;
      wrapped->SetResourceRecord(record);

      record->AddChunk(scope.Get());

      if(pDesc->Type == D3D12_QUERY_HEAP_TYPE_OCCLUSION)
        GetResourceManager()->MarkDirtyResource(wrapped->GetResourceID());
    }

    *ppvHeap = (ID3D12QueryHeap *)wrapped;
  }
  else
  {
    CHECK_HR(this, ret);
  }

  return ret;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::ResolveQueryData(ID3D12QueryHeap *pQueryHeap,
                                                                D3D12_QUERY_TYPE Type,
                                                                UINT StartIndex, UINT NumQueries,
                                                                void *pResolvedQueryData)
{
  // pass through, without serialising
  return m_pDevice15->ResolveQueryData(Unwrap(pQueryHeap), Type, StartIndex, NumQueries,
                                       pResolvedQueryData);
}

INSTANTIATE_FUNCTION_SERIALISED(void, WrappedID3D12Device, CreateQueryHeap1,
                                const D3D12_QUERY_HEAP_DESC *pDesc, D3D12_QUERY_HEAP_FLAGS Flags,
                                REFIID riid, void **ppvHeap);
