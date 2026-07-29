API Reference: Replay Control
=============================

This is the API reference for the functions, classes, and enums in the ``noobdawn`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: noobdawn

Initialisation and Shutdown
---------------------------

.. autofunction:: noobdawn.InitialiseReplay
.. autofunction:: noobdawn.ShutdownReplay

.. autoclass:: noobdawn.GlobalEnvironment
  :members:

.. autoclass:: noobdawn.ResultCode
  :members:

.. autoclass:: noobdawn.ResultDetails
  :members:

Capture File Access
-------------------

.. autofunction:: noobdawn.OpenCaptureFile

.. autoclass:: noobdawn.CaptureAccess
  :members:

.. autoclass:: noobdawn.CaptureFile
  :members:

.. autoclass:: noobdawn.ReplaySupport
  :members:

.. autoclass:: noobdawn.CaptureFileFormat
  :members:

.. autoclass:: noobdawn.SectionProperties
  :members:

.. autoclass:: noobdawn.SectionType
  :members:

.. autoclass:: noobdawn.SectionFlags
  :members:

.. autoclass:: noobdawn.Thumbnail
  :members:

GPU Enumeration
---------------

.. autoclass:: noobdawn.GPUDevice
  :members:

.. autoclass:: noobdawn.GPUVendor
  :members:

.. autofunction:: noobdawn.GPUVendorFromPCIVendor

.. autoclass:: noobdawn.GraphicsAPI
  :members:

.. autofunction:: noobdawn.IsD3D

.. autofunction:: noobdawn.GetDriverInformation

.. autoclass:: noobdawn.DriverInformation
  :members:

Replay Controller
-----------------

.. autoclass:: noobdawn.ReplayController
  :members:

.. autoclass:: noobdawn.ReplayOptions
  :members:

.. autoclass:: noobdawn.ReplayOptimisationLevel
  :members:

.. autoclass:: noobdawn.APIProperties
  :members:

Device Protocols
----------------

.. autoclass:: noobdawn.DeviceProtocolController
  :members:

.. autofunction:: noobdawn.GetSupportedDeviceProtocols
.. autofunction:: noobdawn.GetDeviceProtocolController

Remote Servers
--------------

.. autoclass:: noobdawn.RemoteServer
  :members:

.. autofunction:: noobdawn.CreateRemoteServerConnection
.. autofunction:: noobdawn.CheckRemoteServerConnection
.. autofunction:: noobdawn.BecomeRemoteServer

.. autoclass:: noobdawn.PathEntry
  :members:

.. autoclass:: noobdawn.PathProperty
  :members:
