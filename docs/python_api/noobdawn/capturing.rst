API Reference: Capturing
========================

This is the API reference for the functions, classes, and enums in the ``noobdawn`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: noobdawn

Execution & Injection
---------------------

.. autofunction:: noobdawn.ExecuteAndInject
.. autofunction:: noobdawn.InjectIntoProcess

.. autoclass:: noobdawn.CaptureOptions
  :members:
  
.. autofunction:: noobdawn.GetDefaultCaptureOptions

.. autoclass:: noobdawn.EnvironmentModification
  :members:

.. autoclass:: noobdawn.EnvMod
  :members:

.. autoclass:: noobdawn.EnvSep
  :members:

.. autoclass:: noobdawn.ExecuteResult
  :members:

Global Hooking
--------------

.. autofunction:: noobdawn.StartGlobalHook
.. autofunction:: noobdawn.StopGlobalHook
.. autofunction:: noobdawn.IsGlobalHookActive
.. autofunction:: noobdawn.CanGlobalHook

Target Control
--------------

.. autofunction:: noobdawn.EnumerateRemoteTargets
.. autofunction:: noobdawn.CreateTargetControl

.. autoclass:: noobdawn.TargetControl
  :members:

.. autoclass:: noobdawn.TargetControlMessage
  :members:

.. autoclass:: noobdawn.TargetControlMessageType
  :members:

.. autoclass:: noobdawn.NewCaptureData
  :members:

.. autoclass:: noobdawn.APIUseData
  :members:

.. autoclass:: noobdawn.BusyData
  :members:

.. autoclass:: noobdawn.NewChildData
  :members:

