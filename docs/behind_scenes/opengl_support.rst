OpenGL & OpenGL ES Support
==========================

This page documents the support of OpenGL & OpenGL ES in NoobDawn. This gives an overview of what NoobDawn is capable of, and primarily lists information that is relevant. You might also be interested in the :doc:`full list of features <../getting_started/features>`.

Capture requirements
--------------------

NoobDawn only supports the core profile of OpenGL - from 3.2 up to 4.6 inclusive. This means any compatibility profile functionality will generally not be supported. There are a couple of concessions where it was easy to do so - like allowing the use of VAO 0, or luminance/intensity formats, but this in general will not happen.

.. note::

   To be more compatible with applications, NoobDawn will still attempt to capture on a compatibility context, but it will not replay successfully unless the given subset of functionality is used.

On OpenGL ES, any context version 2.0 and above is supported.

Replay requirements
-------------------

NoobDawn assumes a certain minimum feature set on replay. On desktop this means you must be able to create a 3.2 core context.

Also note that this is the *minimum* required functionality to replay, some analysis features will be disabled unless you have more capable hardware features such as GL_ARB_shader_image_load_store, GL_ARB_compute_shader, ARB_shader_storage_buffer_object, and GL_ARB_gpu_shader5.

On OpenGL ES, you must be able to create a GLES 3 context to replay.

Multiple contexts & multithreading
----------------------------------

NoobDawn assumes that all GL commands (with the exception of perhaps a SwapBuffers call) for frames will come from a single thread. This means that e.g. if commands come from a second thread during loading, or some time during initialisation, this will be supported. However during frame capture all commands are serialised as if they come from a single thread, so interleaved rendering commands from multiple threads will not work.

Extension support
-----------------

NoobDawn supports many ARB, EXT and other vendor-agnostic extensions - primarily those that are either very widespread and commonly used but aren't in core, or are quite simple to support. In general NoobDawn won't support extensions unless they match one of these requirements, and this means most vendor extensions will not be supported.

OpenGL shader debugging
-----------------------

NoobDawn supports a limited form of shader debugging on OpenGL, for vertex, pixel and compute shaders. However not every shader can be debugged and this will vary depending primarily on how modern the shader in question is. Generally OpenGL as a legacy API is not well designed for modern features and expectations should be set appropriately about how widely this feature can be used, and it is possible that there will be many cases where it can't feasibly be made to work.

If your shader compiles unmodified to SPIR-V then it should debug correctly. In some cases NoobDawn will attempt to compile with looser requirements on SPIR-V mapping and handle any differences internally, but this is not guaranteed to work.

In some cases a shader compiled to SPIR-V may invoke undefined behaviour due to differences to the driver compiled GLSL and differences in what resources and bindings are available. The effects of this may vary and best results will come from shaders that are explicitly laid out and explicitly fully bound compared to shaders that are lazily bound.

Shader debugging has high minimum requirements for functionality and so may not be available on all drivers. At least ``ARB_compute_shader`` and ``ARB_shader_storage_buffer_object`` are required.

Android
-------

OpenGL ES capture and replay on Android is natively supported. For more information on how to capture with Android see :doc:`../how/how_android_capture`.

On many drivers debugging vertex shaders will not be possible due to lack of driver feature support.

OS X
----

OS X is not supported.

See Also
--------

* :doc:`../getting_started/features`
