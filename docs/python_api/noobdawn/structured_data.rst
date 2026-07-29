API Reference: Structured Data
==============================

This is the API reference for the functions, classes, and enums in the ``noobdawn`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: noobdawn

Type information
----------------

.. autoclass:: SDType
  :members:

.. autoclass:: SDBasic
  :members:

.. autoclass:: SDTypeFlags
  :members:

Objects
-------

.. autoclass:: SDObject
  :members:

.. autoclass:: SDObjectData
  :members:

.. autoclass:: SDObjectPODData
  :members:

Chunks
------

.. autoclass:: SDChunk
  :members:

.. autoclass:: SDChunkMetaData
  :members:

.. autoclass:: SDChunkFlags
  :members:

Structured File
---------------

.. autoclass:: SDFile
  :members:

Creation Helper Functions
-------------------------

.. autofunction:: noobdawn.makeSDArray
.. autofunction:: noobdawn.makeSDBool
.. autofunction:: noobdawn.makeSDEnum
.. autofunction:: noobdawn.makeSDFloat
.. autofunction:: noobdawn.makeSDInt32
.. autofunction:: noobdawn.makeSDInt64
.. autofunction:: noobdawn.makeSDResourceId
.. autofunction:: noobdawn.makeSDString
.. autofunction:: noobdawn.makeSDStruct
.. autofunction:: noobdawn.makeSDUInt32
.. autofunction:: noobdawn.makeSDUInt64
