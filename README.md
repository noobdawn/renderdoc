<p align="center"><img src="https://user-images.githubusercontent.com/661798/36482670-f81601c0-170b-11e8-8adb-2365b346ac27.png" /></p>

[![MIT licensed](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE.md)
[![CI](https://github.com/baldurk/renderdoc/workflows/CI/badge.svg?branch=v1.x&event=push)](https://github.com/baldurk/renderdoc/actions)
[![Contributor Covenant](https://img.shields.io/badge/Contributor%20Covenant-v2.0%20adopted-ff69b4.svg)](docs/CODE_OF_CONDUCT.md) 

RenderDoc is a frame-capture based graphics debugger, currently available for Vulkan, D3D11, D3D12, OpenGL, and OpenGL ES development on Windows, Linux, Android, and Nintendo Switch&trade;. It is completely open-source under the MIT license.

RenderDoc is intended for debugging your own programs only. Any discussion of capturing programs that you did not create will not be allowed in any official public RenderDoc setting, including the issue tracker, discord, or via email. For example this includes capturing commercial games that you did not create, or capturing Google Maps or Google Earth. Note: Capturing projects you created that use a third party engine like Unreal or Unity, or open source and free projects is completely fine and supported.

If you have any questions, suggestions or problems or you can [create an issue](https://github.com/baldurk/renderdoc/issues/new/choose) here on github, [email me directly](mailto:baldurk@baldurk.org) or come into [IRC](https://webchat.oftc.net/?channels=renderdoc) or [Discord](https://discord.gg/ahq6yRB) to discuss it.

To install on windows run the appropriate installer for your OS ([64-bit](https://renderdoc.org/stable/latest/RenderDoc_latest_64.msi) | [32-bit](https://renderdoc.org/stable/latest/RenderDoc_latest_32.msi)) or download the portable zip from the [builds page](https://renderdoc.org/builds). The 64-bit windows build fully supports capturing from 32-bit programs. On linux only 64-bit x86 is supported - there is a precompiled [binary tarball](https://renderdoc.org/stable/latest/renderdoc_latest.tar.gz) available, or your distribution may package it. If not you can [build from source](docs/CONTRIBUTING/Compiling.md).

* **Downloads**: Stable and nightly builds: https://renderdoc.org/builds ( [Symbol server](https://renderdoc.org/symbols) )
* **Documentation**: [HTML online](https://renderdoc.org/docs), [CHM in builds](https://renderdoc.org/docs/renderdoc.chm), [Videos](https://www.youtube.com/user/baldurkarlsson)
* **Contact**: [baldurk@baldurk.org](mailto:baldurk@baldurk.org), [#renderdoc on OFTC IRC](https://webchat.oftc.net/?channels=renderdoc), [Discord server](https://discord.gg/ahq6yRB)
* **Code of Conduct**: [Contributor Covenant](docs/CODE_OF_CONDUCT.md)
* **Information for contributors**: [All contribution information](docs/CONTRIBUTING.md), [Compilation instructions](docs/CONTRIBUTING/Compiling.md)
* **Community extensions**: [Extensions repository](https://github.com/baldurk/renderdoc-contrib)

## Modification Notes

In this forked version, I have made the following modifications to the original project:

### New Features

1. **Blacklist Feature**: Added a blacklist feature to manage child processes that do not need to be hooked.
   - If the process name of the child process is on the blacklist, it will not attempt to hook.
   - If multiple process names are added to the blacklist, please use a semicolon as a separator.
   - Sometimes attempting to hook child processes can cause some unpredictable obstacles for the child processes, even if those child processes are not the target of the hook. Adding a blacklist to manage these completely ignorable child processes can help the program start correctly.

License
--------------

RenderDoc is released under the MIT license, see [LICENSE.md](LICENSE.md) for full text as well as 3rd party library acknowledgements.

Compiling
---------

Building RenderDoc is fairly straight forward on most platforms. See [Compiling.md](docs/CONTRIBUTING/Compiling.md) for more details.

Contributing & Development
--------------

I've added some notes on how to contribute, as well as where to get started looking through the code in [Developing-Change.md](docs/CONTRIBUTING/Developing-Change.md). All contribution information is available under [CONTRIBUTING.md](docs/CONTRIBUTING.md).

