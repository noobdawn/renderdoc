API Reference: Shaders
======================

This is the API reference for the functions, classes, and enums in the ``noobdawn`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: noobdawn

Descriptors
-----------

.. autoclass:: noobdawn.Descriptor
  :members:

.. autoclass:: noobdawn.SamplerDescriptor
  :members:

.. autoclass:: noobdawn.DescriptorFlags
  :members:
   
.. autoclass:: noobdawn.DescriptorCategory
  :members:
   
.. autoclass:: noobdawn.DescriptorType
  :members:
   
.. autofunction:: noobdawn.CategoryForDescriptorType
.. autofunction:: noobdawn.IsConstantBlockDescriptor
.. autofunction:: noobdawn.IsReadOnlyDescriptor
.. autofunction:: noobdawn.IsReadWriteDescriptor
.. autofunction:: noobdawn.IsSamplerDescriptor

.. autoclass:: noobdawn.DescriptorLogicalLocation
  :members:

.. autoclass:: noobdawn.DescriptorRange
  :members:

.. autoclass:: noobdawn.DescriptorAccess
  :members:

Reflection
----------

.. autoclass:: noobdawn.ShaderReflection
  :members:

.. autoclass:: noobdawn.ShaderStage
  :members:

.. autoclass:: noobdawn.ShaderStageMask
  :members:

.. autofunction:: noobdawn.MaskForStage
.. autofunction:: noobdawn.FirstStageForMask

.. autoclass:: noobdawn.SigParameter
  :members:

.. autoclass:: noobdawn.ShaderBuiltin
  :members:

.. autoclass:: noobdawn.ConstantBlock
  :members:

.. autoclass:: noobdawn.ShaderSampler
  :members:

.. autoclass:: noobdawn.ShaderResource
  :members:

Debug Info
----------

.. autoclass:: noobdawn.ShaderDebugInfo
  :members:

.. autoclass:: noobdawn.ShaderEncoding
  :members:
  
.. autoclass:: noobdawn.KnownShaderTool
  :members:
  
.. autofunction:: noobdawn.ToolExecutable
.. autofunction:: noobdawn.ToolInput
.. autofunction:: noobdawn.ToolOutput

.. autofunction:: noobdawn.IsTextRepresentation

.. autoclass:: noobdawn.ShaderEntryPoint
  :members:

.. autoclass:: noobdawn.ShaderSourceFile
  :members:

.. autoclass:: noobdawn.ShaderCompileFlags
  :members:

.. autoclass:: noobdawn.ShaderCompileFlag
  :members:

.. autoclass:: noobdawn.ShaderSourcePrefix
  :members:

Shader Constants
----------------

.. autoclass:: noobdawn.ShaderConstant
  :members:

.. autoclass:: noobdawn.ShaderConstantType
  :members:

.. autoclass:: noobdawn.ShaderVariableFlags
  :members:

.. autoclass:: noobdawn.VarType
  :members:

.. autofunction:: noobdawn.VarTypeByteSize
.. autofunction:: noobdawn.VarTypeCompType

Shader Debugging
----------------

.. autoclass:: noobdawn.ShaderDebugTrace
  :members:

.. autoclass:: noobdawn.ShaderDebugger
  :members:

.. autoclass:: noobdawn.SourceVariableMapping
  :members:

.. autoclass:: noobdawn.DebugVariableReference
  :members:

.. autoclass:: noobdawn.DebugVariableType
  :members:

.. autoclass:: noobdawn.LineColumnInfo
  :members:

.. autoclass:: noobdawn.InstructionSourceInfo
  :members:

.. autoclass:: noobdawn.ShaderDebugState
  :members:

.. autoclass:: noobdawn.ShaderEvents
  :members:

.. autoclass:: noobdawn.ShaderVariableChange
  :members:

Shader Variables
----------------
  
.. autoclass:: noobdawn.ShaderVariable
  :members:

.. autoclass:: noobdawn.ShaderValue
  :members:

.. autoclass:: noobdawn.PointerVal
  :members:

.. autoclass:: noobdawn.ShaderBindIndex
  :members:

.. autoclass:: noobdawn.ShaderDirectAccess
  :members:
