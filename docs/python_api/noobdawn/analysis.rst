API Reference: Replay Analysis
==============================

This is the API reference for the functions, classes, and enums in the ``noobdawn`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: noobdawn

Frame and Actions
-----------------

.. autoclass:: FrameDescription
  :members:

.. autoclass:: ActionDescription
  :members:

.. autoclass:: ActionFlags
  :members:

.. autoclass:: APIEvent
  :members:

Debug Messages
--------------

.. autoclass:: DebugMessage
  :members:

.. autoclass:: MessageCategory
  :members:

.. autoclass:: MessageSeverity
  :members:

.. autoclass:: MessageSource
  :members:

Resource Usage
--------------

.. autoclass:: EventUsage
  :members:

.. autoclass:: ResourceUsage
  :members:

.. autofunction:: noobdawn.ResUsage
.. autofunction:: noobdawn.RWResUsage
.. autofunction:: noobdawn.CBUsage

Texture Saving
--------------

.. autoclass:: TextureSave
  :members:

.. autoclass:: FileType
  :members:

.. autoclass:: AlphaMapping
  :members:

.. autoclass:: TextureComponentMapping
  :members:

.. autoclass:: TextureSampleMapping
  :members:

.. autoclass:: TextureSliceMapping
  :members:

Pixel History
-------------

.. autoclass:: PixelModification
  :members:

.. autoclass:: ModificationValue
  :members:

.. autoclass:: PixelValue
  :members:

Shader Debuging
---------------

.. autoclass:: DebugPixelInputs
  :members:
