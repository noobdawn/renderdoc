API Reference: Replay Outputs
=============================

This is the API reference for the functions, classes, and enums in the ``noobdawn`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: noobdawn

General
-------

.. autoclass:: ReplayOutput
  :members:

.. autoclass:: ReplayOutputType
  :members:

.. autofunction:: noobdawn.SetColors

Window Configuration
--------------------

.. autoclass:: WindowingData
  :members:

.. autoclass:: WindowingSystem
  :members:

.. autofunction:: noobdawn.CreateHeadlessWindowingData
.. autofunction:: noobdawn.CreateWin32WindowingData
.. autofunction:: noobdawn.CreateXlibWindowingData
.. autofunction:: noobdawn.CreateXCBWindowingData
.. autofunction:: noobdawn.CreateWaylandWindowingData
.. autofunction:: noobdawn.CreateAndroidWindowingData
.. autofunction:: noobdawn.CreateMacOSWindowingData

Texture View
------------

.. autoclass:: TextureDisplay
  :members:

.. autoclass:: DebugOverlay
  :members:

Mesh View
---------

.. autoclass:: MeshDisplay
  :members:

.. autoclass:: MeshDataStage
  :members:

.. autoclass:: MeshletSize
  :members:

.. autoclass:: TaskGroupSize
  :members:

.. autoclass:: MeshFormat
  :members:

.. autoclass:: Visualisation
  :members:

.. autoclass:: Camera
  :members:

.. autoclass:: CameraType
  :members:

.. autoclass:: AxisMapping
  :members:

.. autofunction:: noobdawn.InitCamera
