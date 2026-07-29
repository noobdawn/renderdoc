API Reference: Common Pipeline State Abstraction
================================================

This is the API reference for the functions, classes, and enums in the ``noobdawn`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../../index`.

.. contents:: Sections
   :local:

.. currentmodule:: noobdawn

.. autoclass:: PipeState
  :members:

General
-------

.. autoclass:: noobdawn.Offset
  :members:

Vertex Inputs
-------------

.. autoclass:: noobdawn.BoundVBuffer
  :members:

.. autoclass:: noobdawn.VertexInputAttribute
  :members:

.. autoclass:: noobdawn.Topology
  :members:
  
.. autofunction:: noobdawn.NumVerticesPerPrimitive
.. autofunction:: noobdawn.VertexOffset
.. autofunction:: noobdawn.PatchList_Count
.. autofunction:: noobdawn.PatchList_Topology
.. autofunction:: noobdawn.IsStrip

Shader Resource Bindings
------------------------

.. autoclass:: noobdawn.UsedDescriptor
  :members:

.. autoclass:: noobdawn.BindType
  :members:

.. autoclass:: noobdawn.TextureSwizzle
  :members:

.. autoclass:: noobdawn.TextureSwizzle4
  :members:

Samplers
--------

.. autoclass:: noobdawn.AddressMode
  :members:

.. autoclass:: noobdawn.TextureFilter
  :members:

.. autoclass:: noobdawn.FilterMode
  :members:
  
.. autoclass:: noobdawn.FilterFunction
  :members:

.. autoclass:: noobdawn.ChromaSampleLocation
  :members:

.. autoclass:: noobdawn.YcbcrConversion
  :members:

.. autoclass:: noobdawn.YcbcrRange
  :members:

Viewport and Scissor
--------------------

.. autoclass:: noobdawn.Viewport
  :members:

.. autoclass:: noobdawn.Scissor
  :members:

Rasterizer
----------

.. autoclass:: noobdawn.CullMode
  :members:

.. autoclass:: noobdawn.DepthBiasMode
  :members:

.. autoclass:: noobdawn.FillMode
  :members:

.. autoclass:: noobdawn.ConservativeRaster
  :members:

.. autoclass:: noobdawn.LineRaster
  :members:

.. autoclass:: noobdawn.ShadingRateCombiner
  :members:

.. autoclass:: noobdawn.RasterState
  :members:


Depth and Stencil
-----------------

.. autoclass:: noobdawn.DepthTestState
  :members:

.. autoclass:: noobdawn.StencilFace
  :members:

.. autoclass:: noobdawn.StencilOperation
  :members:

.. autoclass:: noobdawn.CompareFunction
  :members:

Blending
--------

.. autoclass:: noobdawn.ColorBlend
  :members:

.. autoclass:: noobdawn.BlendEquation
  :members:

.. autoclass:: noobdawn.BlendMultiplier
  :members:

.. autoclass:: noobdawn.BlendOperation
  :members:

.. autoclass:: noobdawn.LogicOperation
  :members:

Shader Messages
---------------

.. autoclass:: noobdawn.ShaderMessage
  :members:

.. autoclass:: noobdawn.ShaderMessageLocation
  :members:

.. autoclass:: noobdawn.ShaderMeshMessageLocation
  :members:

.. autoclass:: noobdawn.ShaderVertexMessageLocation
  :members:

.. autoclass:: noobdawn.ShaderPixelMessageLocation
  :members:

.. autoclass:: noobdawn.ShaderGeometryMessageLocation
  :members:

.. autoclass:: noobdawn.ShaderComputeMessageLocation
  :members:


* qnoobdawn.ShaderMessageViewer
