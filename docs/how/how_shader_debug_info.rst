How do I use shader debug information?
======================================

NoobDawn makes extensive use of shader reflection and debug information to provide a smoother debugging experience. This kind of information gives names to anything in the shader interface such as constants, resource bindings, interpolated inputs and outputs. It includes better type information that in many cases is not available, and it can also even include embedded copies of the shader source which is used for source-level debugging.

On most APIs this debug information adds up to a large amount of information which is not necessary for functionality, and so it can be optionally stripped out. Many shader compilation pipelines will do this automatically, so the information is lost by the time NoobDawn can intercept it at the API level. For this reason there are several ways to save either save the unstripped shader blob or just the debug information separately at compile time and provide ways for NoobDawn to correlate the stripped blob it sees passed to the API with the debug information on disk.

If you did not deliberately strip out debug information and left it embedded in the blob, you do not need to do anything else and NoobDawn will find and use the information. If you used a compiler-based method of generating separate debug information you may need to specify search paths in the NoobDawn options but you do not need to follow any of these steps.

.. note::

  OpenGL is effectively excluded from this consideration because it has no separate debug information, everything is generated from the source uploaded to the API. If the source has been stripped of information or obfuscated, this will have to be handled by your application.

.. warning::

  Since this debug information is stored separately, it is *not* part of the capture and so if the debug information is moved or deleted NoobDawn will not be able to find it and the capture will show only what is possible with no debug information.

Shader search paths
-------------------

In the NoobDawn settings menu, under the ``Core`` category, you can specify shader debug search paths. These are the directories that will be searched to find separated debug information based on a path in the original shader.

Each path can be set as 'recursive' or not, with the default being to treat it as recursive. This is explained below in the search priority list, but generally should be turned off for network shares or very large folders where listing all contained files recusively would be slow. The paths can be rearranged to provide a priority order.

When searching for separate debug info based on a path in the stripped shader blob, NoobDawn follows the following algorithm. This is based on trying to match PIX's behaviour which is the primary other tool that supports this, under the principle of least surprise. PIX's search algorithm is deliberately undocumented and so this has been determined by reverse engineering, some tweaks have been made for usability.

.. note::

  If the filename is proceeded by ``lz4#`` then this will be stripped before consideration and the file will be considered lz4 compressed. This is a NoobDawn extension only possible when using manually-specified shader blobs and is not currently supported by any compiler.

In this algorithm the original path from the shader is referred to as a 'filename', but it may contain relative path elements and may not be only a filename.

If the filename does not end in ``.pdb`` or ``.PDB`` then each time a file is checked, if it doesn't exist then the suffix will be appended and checked again. This is for compatibility with PIX.

If a file is found and the original shader has a debug hash for matching then that will be checked against the debug hash in the debug information. If there's no match this is assumed to be a false positive and the search continues. Whether such a debug hash exists is compiler specific and may only happen in certain compiler versions (e.g. older versions of dxc do not produce this hash).

1. First if the filename is absolute, that file will be checked first.
2. Next, each path specified in the search paths will be checked in order, appending the filename to the current search path.
3. If no match is found and the filename does contain relative path entries, such as ``foo/bar/filename.pdb`` then the first entry will be dropped to produce ``bar/filename.pdb`` and the search paths will be checked again. This is for compatibility with PIX.
4. If all search paths have been checked and the filename is just a filename with no path entries, then a recursive search happens. All search paths that are marked as 'recursive' will be searched exhaustively for all files under that path in any subdirectory via a depth-first search. This is for compatibility with PIX.
5. The first time the desired filename is encountered, that debug information will be checked. If the debug information is not a match the search will stop and will not continue.
6. It is believed that PIX will inspect all files at this stage for debug hash matches regardless of filename, NoobDawn does not do that for performance considerations.

Manually Specifying debug shader information
--------------------------------------------

This section is about manually separating out and specifying shader debug information. If you use compiler-native methods such as the ``-Fd`` flag you can ignore this.

Separated debug shader blobs can be specified using a path and an API-specific mechanism, if the separation has been done manually. The path itself can either be an absolute path, which will be used directly every time, or it can be a relative path which allows the blob identifier to be specified relative to customisable search folders. If using a relative path, it can be as simple as a filename.

The search folders for shader debug info are specified in the settings window, under the ``Core`` category. You can configure as many search directories as you wish, and they will be searched in the listed order according to the above search algorithm.

Using the D3D11 API you can specify the path at runtime:

.. highlight:: c++
.. code:: c++

    std::string pathName = "path/to/saved/blob.dxbc"; // path name is in UTF-8

    ID3D11VertexShader *shader = ...;

    // GUID value in noobdawn_app.h
    GUID NOOBDAWN_ShaderDebugMagicValue = NOOBDAWN_ShaderDebugMagicValue_struct;

    // string parameter must be NULL-terminated, and in UTF-8
    shader->SetPrivateData(NOOBDAWN_ShaderDebugMagicValue,
                           (UINT)pathName.length(), pathName.c_str());

You can also specify it using the Vulkan API:

.. highlight:: c++
.. code:: c++

    std::string pathName = "path/to/saved/blob.dxbc"; // path name is in UTF-8

    VkShaderModule shaderModule = ...;

    // Both EXT_debug_marker and EXT_debug_utils can be used, this example uses
    // EXT_debug_utils as EXT_debug_marker is deprecated
    VkDebugUtilsObjectTagInfoEXT tagInfo = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT};
    tagInfo.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
    tagInfo.objectHandle = (uint64_t)shaderModule;
    // tag value in noobdawn_app.h
    tagInfo.tagName = NOOBDAWN_ShaderDebugMagicValue_truncated;
    tagInfo.pTag = pathName.c_str();
    tagInfo.tagSize = pathName.length();

    vkSetDebugUtilsObjectTagEXT(device, &tagInfo);

On D3D11 and D3D12 it is also possible to set the path using the ``PRIV`` private data section in the DXBC container, either when using fxc or dxc. To do this set the above GUID as the first 16 bytes and then the path as a NULL terminated string immediately following it.

See Also
--------

* :doc:`how_debug_shader`
