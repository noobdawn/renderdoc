D3D12 Support
=============

This page documents the support of D3D12 in NoobDawn. This gives an overview of what NoobDawn is capable of, and primarily lists information that is relevant. You might also be interested in the :doc:`full list of features <../getting_started/features>`.

Performance notes
-----------------

D3D12 is intended as to have low CPU overhead and be fully threadable, and NoobDawn strives to maintain that performance as much as possible. While some overhead is inevitable NoobDawn aims to have no locks on the 'hot path' of command buffer recording, minimal or no allocation, and in general to have low performance overhead while not capturing.

Some patterns of access are more or less conducive to good performance on NoobDawn, so if you are having trouble with slow capture, large memory/disk overhead or slow replay you might want to try eliminating use of persistent maps of resources. If you do have persistent maps, ensure that either the memory allocation is small or that you have only a few queue submits during the frame - since NoobDawn must compare the whole allocation at every submit to determine what might have changed and save out the delta.

Likewise try to avoid making very large memory allocations in the range of 1GB and above. By its nature NoobDawn must save one or more copies of memory allocations to enable proper capture, so having allocations limited to only a few 100s of megabytes can help gain granularity of management and limit the memory overhead NoobDawn adds. There may be optimisation of this in future on NoobDawn's side but there are no easy guarantees.

Current support
---------------

NoobDawn has initial support for D3D12, but it contains some caveats. In addition, not all replay features are currently supported, but this will eventually reach parity with other APIs.

* NoobDawn assumes that even if multiple GPUs are present, that only one will be used - i.e. NodeMask is always 0. Multiple queues are supported.
* NoobDawn captures may not be portable between different systems, only currently supporting capture and replay on the same or similar enough machines.

For more information about NoobDawn's raytracing support see :doc:`raytracing`.

NoobDawn extensions
--------------------

On D3D12 there is a NoobDawn extension provided with this interface, queried from an ``ID3D12DescriptorHeap``:

.. highlight:: c++
.. code:: c++

  MIDL_INTERFACE("52528c37-bfd9-4bbb-99ff-fdb7188619ce")
  INoobDawnDescriptorNamer : public IUnknown
  {
  public:
    virtual HRESULT STDMETHODCALLTYPE SetName(UINT DescriptorIndex, LPCSTR Name) = 0;
  };

This interface allows you to set a custom name for descriptors, if using SM6.6 style ``ResourceDescriptorHeap[]`` access for better debugging.

See Also
--------

* :doc:`../getting_started/features`
