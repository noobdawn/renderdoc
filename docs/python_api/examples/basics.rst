Basic Interfaces
================

This document explains some common interfaces and their relationship, which can be useful as a primer to understand where to get started, as well as for reference to look back on from later examples.

Replay Basics
-------------

ReplayController
^^^^^^^^^^^^^^^^

The primary interface for accessing the low level of NoobDawn's replay analysis is :py:class:`~noobdawn.ReplayController`.

From this interface, information about the capture can be gathered, using e.g. :py:meth:`~noobdawn.ReplayController.GetRootActions` to return the list of root-level actions in the frame, or :py:meth:`~noobdawn.ReplayController.GetResources` to return a list of all resources in the capture.

Some methods like the two above return information which is global and does not vary across the frame. Most functions however return information relative to the current point in the frame.

During NoobDawn's replay, you can imagine a cursor that moves back and forth between the start and end of the frame. All requests for information that varies - such as texture and buffer contents, pipeline state, and other information will be relative to the current event.

Every function call within a frame is assigned an ascending ``eventId``, from ``1`` up to as many events as are in the frame. Within the action list returned by :py:meth:`~noobdawn.ReplayController.GetRootActions`, each action contains a list of events in :py:attr:`~noobdawn.ActionDescription.events`. These contain all of the ``eventId`` that immediately preceeded the action. The details of the function call can be found by using :py:attr:`~noobdawn.APIEvent.chunkIndex` as an index into the structured data returned from :py:meth:`~noobdawn.GetStructuredFile`. The structured data contains the function name and the complete set of parameters passed to it, with their values.

To change the current active event and move the cursor, you can call :py:meth:`~noobdawn.ReplayController.SetFrameEvent`. This will move the replay to represent the current state immediately after the given event has executed.

At this point you can use :py:meth:`~noobdawn.ReplayController.GetBufferData` and :py:meth:`~noobdawn.ReplayController.GetTextureData` to obtain the contents of a buffer or texture respectively. The pipeline state can be accessed via ``Get*PipelineState`` for each API - to determine the current capture's pipeline type you can fetch the API properties from :py:meth:`~noobdawn.ReplayController.GetAPIProperties`.

There is also an API-agnostic pipeline abstraction to return information that is the same across APIs. Using :py:meth:`~noobdawn.GetPipelineState` returns a :py:class:`~noobdawn.PipeState` which has accessors for fetching the current vertex buffers, shaders, and colour outputs. This allows you to write generic code that will work on any API that NoobDawn supports. The API-specific pipelines are still available through ``Get*PipelineState``.

For more examples of how to fetch data, see the concrete examples below.

ReplayOutput
^^^^^^^^^^^^

While :py:class:`~noobdawn.ReplayController` provides methods for obtaining data directly, it doesn't provide any functionality for displaying to a window. For this the :py:class:`~noobdawn.ReplayOutput` class allows you to bind to a window and configure the output.

First you need to gather the platform-specific windowing information in a :py:class:`~noobdawn.WindowingData`. This class is opaque to python, but you can create it using helper functions such as :py:func:`~noobdawn.CreateWin32WindowingData` and :py:func:`~noobdawn.CreateXlibWindowingData`. The parameters to these are platform specific, and are typically accepted as integers where they refer to a windowing handle.

To then create an output for a window, :py:meth:`~noobdawn.ReplayController.CreateOutput` can create different types of outputs.

Once created, you can configure the output with :py:meth:`~noobdawn.ReplayOutput.SetMeshDisplay` and :py:meth:`~noobdawn.ReplayOutput.SetTextureDisplay` to update the configuration, and then call :py:meth:`~noobdawn.ReplayOutput.Display` to display on screen.

.. _qnoobdawn-python-basics:

NoobDawn UI Basics
-------------------

The NoobDawn UI provides a number of useful abstractions over the lower level API, which can be convenient when developing scripts. In addition it gives access to the different panels to allow limited control over them. The ``pynoobdawn`` global is available to all scripts running within the NoobDawn UI, and it provides access to all of these things.

Each single-instance panel such as the :py:class:`~qnoobdawn.TextureViewer` or :py:class:`~qnoobdawn.PipelineStateViewer` has accessors within the :py:class:`~qnoobdawn.CaptureContext`.

Functions such as :py:meth:`~qnoobdawn.CaptureContext.GetTextureViewer` will return a valid handle to the texture viewer, but if the texture viewer was closed then although it will be created it will *not* be immediately visible. You need to call :py:meth:`~qnoobdawn.CaptureContext.ShowTextureViewer` first which will bring the texture viewer to the front and make sure it is visible and docked if it wasn't already.

You can also create new instances of windows such as buffer or shader viewers using :py:meth:`~qnoobdawn.CaptureContext.ViewBuffer` or :py:meth:`~qnoobdawn.CaptureContext.ViewShader`.

The :py:class:`~qnoobdawn.CaptureContext` interface also provides useful utility functions such as :py:meth:`~qnoobdawn.CaptureContext.GetTexture` or :py:meth:`~qnoobdawn.CaptureContext.GetAction` to look up objects by id instead of needing your own caching and lookup from the lists returned by the lower level interface.
