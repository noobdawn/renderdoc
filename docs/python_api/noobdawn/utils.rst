API Reference: Utilities
========================

This is the API reference for the functions, classes, and enums in the ``noobdawn`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: noobdawn

Maths
-----

.. autoclass:: FloatVector
  :members:

.. autofunction:: noobdawn.HalfToFloat
.. autofunction:: noobdawn.FloatToHalf

Logging & Versioning
--------------------

.. autofunction:: noobdawn.LogMessage
.. autofunction:: noobdawn.SetDebugLogFile
.. autofunction:: noobdawn.GetLogFile
.. autofunction:: noobdawn.GetCurrentProcessMemoryUsage
.. autofunction:: noobdawn.DumpObject

.. autoclass:: LogType
  :members:


Versioning
----------

.. autofunction:: noobdawn.GetVersionString
.. autofunction:: noobdawn.GetCommitHash
.. autofunction:: noobdawn.IsReleaseBuild

Settings
--------

.. autofunction:: noobdawn.GetConfigSetting
.. autofunction:: noobdawn.SetConfigSetting
.. autofunction:: noobdawn.SaveConfigSettings

Self-hosted captures
--------------------

.. autofunction:: noobdawn.CanSelfHostedCapture
.. autofunction:: noobdawn.StartSelfHostCapture
.. autofunction:: noobdawn.EndSelfHostCapture
