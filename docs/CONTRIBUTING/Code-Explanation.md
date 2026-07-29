# Code Explanation

This is a rough 'table of contents' overview of how the main code components are organised:

    noobdawn/ 
        CMakeLists.txt           ; The cmake file, will recurse into subdirectories to build them
        noobdawn.sln            ; VS2015 solution for windows building
        noobdawn/
            3rdparty/            ; third party utilities & libraries included
            drivers/             ; API-specific back-ends, can be individually skipped/removed
            ...                  ; everything else in here consists of the core noobdawn runtime
        noobdawncmd/            ; A small C++ utility program that runs to do various little tasks
        noobdawnshim/           ; A tiny C DLL using only kernel32.dll that is used for global hooking
        qnoobdawn/              ; The Qt UI layer built on top of noobdawn/
        docs/                    ; source documentation for the .chm file or http://docs.noobdawn.org/
        util/                    ; folder for utility/support files - e.g. build scripts, installers, CI config
