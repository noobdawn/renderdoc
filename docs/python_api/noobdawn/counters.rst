API Reference: Performance Counters
===================================

This is the API reference for the functions, classes, and enums in the ``noobdawn`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: noobdawn

Counters
--------

.. autoclass:: noobdawn.CounterDescription
  :members:

.. autoclass:: noobdawn.CounterUnit
  :members:

.. autoclass:: noobdawn.Uuid
  :members:

Counter Types
-------------

.. autoclass:: noobdawn.GPUCounter
  :members:

.. autofunction:: noobdawn.IsAMDCounter
.. autofunction:: noobdawn.IsARMCounter
.. autofunction:: noobdawn.IsGenericCounter
.. autofunction:: noobdawn.IsIntelCounter
.. autofunction:: noobdawn.IsNvidiaCounter
.. autofunction:: noobdawn.IsVulkanExtendedCounter

Results
-------

.. autoclass:: noobdawn.CounterResult
  :members:

.. autoclass:: noobdawn.CounterValue
  :members:
