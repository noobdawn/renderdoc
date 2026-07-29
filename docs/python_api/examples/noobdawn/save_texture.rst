Save a texture to disk
======================

In this example we will find a particular action, and save the color output to disk as an image file.

To begin with, so that we have an interesting action selected we iterate over the list of actions finding the draw with the highest vertex count. For more on how to iterate through a capture's list of actions, see :doc:`iter_actions`.

Once we have set the draw we want as the current event, we can configure the texture save operation. To do this we create a :py:class:`~noobdawn.TextureSave` object. The properties of the object determine how the texture will be mapped to an image file to be saved to disk.

At minimum you need to select a file format, and we'll try a few - :py:attr:`~noobdawn.FileType.JPG`, :py:attr:`~noobdawn.FileType.HDR`, :py:attr:`~noobdawn.FileType.PNG`, and :py:attr:`~noobdawn.FileType.DDS`.

For :py:attr:`~noobdawn.FileType.JPG` and :py:attr:`~noobdawn.FileType.HDR`, alpha is not supported so we choose to blend to a checkerboard pattern in RGB, so the alpha is 'visible'. You can also choose other alpha operations. For the other formats they support alpha natively so we preserve it.

:py:attr:`~noobdawn.FileType.DDS` is the only format that supports mip levels and array slices, so we choose to keep all of these in the output file instead of selecting only one. It is also possible to map array slices into a grid to display an array texture in a single-image format.

:py:attr:`~noobdawn.FileType.DDS` will also support the exact format that the texture is in, rather than encoding it to a different precision.

Example Source
--------------

.. only:: html and not htmlhelp

    :download:`Download the example script <save_texture.py>`.

.. literalinclude:: save_texture.py