Raytracing Support
==================

The primary goal of raytracing handling in NoobDawn is to allow for debugging the rest of the normal work while an application has raytracing enabled. The raytracing work itself will be entirely opaque and there will be no way to introspect any bindings, shaders, or what it is doing at even a basic or rudimentary level - NoobDawn does not support debugging anything to do with raytracing in either Vulkan or D3D12.

Any raytracing work in the captured frames will be recorded and the results accurately reflected in order to allow debugging of any other non-raytracing work that happens as normal. This means the contents of any resources updated by raytracing calls will be correct in subsequent normal graphics work.

At present there is no plan to expand this support, and this is a deliberate decision to make it very clear what is and is not supported in NoobDawn. For this reason the support will not be expanded incrementally to avoid a situation where raytracing debugging exists but is in extremely poor shape. Since raytracing is an entirely separate/dedicated black box subset of APIs the tooling requirements for it are quite different to normal graphics work and would require significant resource investment.

.. note::

    As of the latest versions, nVidia drivers do not support the necessary API functionality to support Vulkan capture of raytracing pipelines. When running on nVidia you will not be able to capture raytracing pipeline work, only ray query work.
