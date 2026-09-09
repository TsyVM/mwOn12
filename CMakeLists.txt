cmake_minimum_required(VERSION 3.20)
project(MWOn12 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MWON12_VERSION "1.0.0")

# ── x86 only — NFSMW speed.exe is 32-bit ─────────────────────────────────────
if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR
   CMAKE_GENERATOR_PLATFORM STREQUAL "AMD64" OR
   (NOT CMAKE_GENERATOR_PLATFORM AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(FATAL_ERROR
        "MWOn12 must be built for Win32 (x86).\n"
        "Use Build.bat, or pass -A Win32 to CMake.")
endif()
message(STATUS "MWOn12 ${MWON12_VERSION} — target: Win32 / x86")

# ── Prerequisites ────────────────────────────────────────────────────────────
# Visual Studio and the Windows 10/11 SDK, and nothing else.
#
# MWDX required the DirectX June 2010 SDK for d3d9.lib and dxguid.lib. MWOn12
# does not: the Windows SDK ships both, and the D3D9 side of this DLL is all
# interface definitions and GUIDs, never a call into the D3D9 runtime. There is
# also no FetchContent here, so a build needs no network.

# ── MWOn12SDK — optional, found next door ────────────────────────────────────
# The SDK is a separate project. If it is present, plugin support is compiled
# in and the ABI header comes from the SDK, which owns it. If it is not, the
# renderer still builds and simply loads no plugins. There is deliberately no
# copy of the header in this tree: two copies drift, and a plugin compiled
# against the wrong one is a crash rather than an error.
if(NOT DEFINED MWON12_SDK_ROOT OR MWON12_SDK_ROOT STREQUAL "")
    foreach(candidate
            "${CMAKE_SOURCE_DIR}/../MWOn12SDK"
            "${CMAKE_SOURCE_DIR}/MWOn12SDK")
        if(EXISTS "${candidate}/include/mwon12/mwon12.h")
            get_filename_component(MWON12_SDK_ROOT "${candidate}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()

if(MWON12_SDK_ROOT AND EXISTS "${MWON12_SDK_ROOT}/include/mwon12/mwon12.h")
    set(MWON12_HAVE_SDK 1)
    message(STATUS "MWOn12SDK: ${MWON12_SDK_ROOT} — plugin support ENABLED")
else()
    set(MWON12_HAVE_SDK 0)
    message(STATUS "MWOn12SDK: not found — plugin support DISABLED. "
                   "Put MWOn12SDK beside this folder, or pass "
                   "-DMWON12_SDK_ROOT=<path>, to enable it.")
endif()

# ── Shared, API-independent core ─────────────────────────────────────────────
set(CORE_SOURCES
    src/DirectX-Handler/core/BackendSelect.cpp
    src/DirectX-Handler/core/CapsTable.cpp
    src/DirectX-Handler/core/D3D9ShaderTranslator.cpp
    src/DirectX-Handler/core/FFPShaderGen.cpp
    src/DirectX-Handler/core/FormatConverter.cpp
    src/DirectX-Handler/core/Log.cpp
    src/DirectX-Handler/core/RenderStateTracker.cpp
    src/DirectX-Handler/core/ShaderMods.cpp
)

# ── DX12 backend ─────────────────────────────────────────────────────────────
# Explicit, not globbed. A new .cpp must be added here or it links as a wall of
# unresolved externals that look like missing code and are not.
set(DX12_SOURCES
    src/DirectX-Handler/dx12/core/Blitter12.cpp
    src/DirectX-Handler/dx12/core/DeviceContext12.cpp
    src/DirectX-Handler/dx12/core/FFPEmulator12.cpp
    src/DirectX-Handler/dx12/core/PSOCache12.cpp
    src/DirectX-Handler/dx12/core/Resource12.cpp
    src/DirectX-Handler/dx12/core/ShaderCache12.cpp
    src/DirectX-Handler/dx12/core/TextureStorage12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/Buffers12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Root12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/Shaders12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12_Draw.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12_Present.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12_Resource.cpp
    src/DirectX-Handler/dx12/d3d9proxy/D9Device12_State.cpp
    src/DirectX-Handler/dx12/d3d9proxy/Textures12.cpp
)

# ── DX9 passthrough (Backend=1 escape hatch) ─────────────────────────────────
set(PASSTHROUGH_SOURCES
    src/DirectX-Handler/directx/WrappedIDirect3D9.cpp
    src/DirectX-Handler/directx/WrappedIDirect3DDevice9.cpp
)

set(SOURCES
    src/dllmain.cpp
    src/Logger.cpp
    src/sdk/PluginHost.cpp
    src/sdk/AsiLoader.cpp
    ${PASSTHROUGH_SOURCES}
    ${CORE_SOURCES}
    ${DX12_SOURCES}
)

add_library(MWOn12_dll SHARED ${SOURCES})

target_include_directories(MWOn12_dll PRIVATE
    src
    src/DirectX-Handler
    src/DirectX-Handler/directx
    src/DirectX-Handler/inc
    src/DirectX-Handler/dx12/inc
)
if(MWON12_HAVE_SDK)
    target_include_directories(MWOn12_dll PRIVATE "${MWON12_SDK_ROOT}/include")
endif()

# The Windows SDK's d3d9.h, dxgi.h and d3dcompiler.h are used rather than the
# June-2010 DXSDK's: the old headers lack DXGI 1.2 types and the 4-argument
# D3DReflect this code needs.
#
# d3d9.lib is deliberately NOT linked. Nothing here calls a D3D9 export -- the
# passthrough resolves them with GetProcAddress -- and linking it would make a
# DLL named d3d9.dll carry an import of d3d9.dll.
target_link_libraries(MWOn12_dll PRIVATE
    d3d12.lib
    dxgi.lib
    d3dcompiler.lib
    dxguid.lib
)

if(MSVC)
    target_compile_options(MWOn12_dll PRIVATE
        $<$<CONFIG:Release>:/MT>
        $<$<CONFIG:Debug>:/MTd>
        /W3
        /wd4996
        /wd4005
    )
endif()

target_compile_definitions(MWOn12_dll PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    MWON12_HAVE_SDK=${MWON12_HAVE_SDK}
    MWON12_VERSION_STRING="${MWON12_VERSION}"
)

# Output is always d3d9.dll, whatever the target is called: that is the name
# the game resolves its imports against.
set_target_properties(MWOn12_dll PROPERTIES
    OUTPUT_NAME "d3d9"
    SUFFIX      ".dll"
    PDB_NAME    "MWOn12"
    LINK_FLAGS  "/DEF:\"${CMAKE_SOURCE_DIR}/MWOn12.def\" /NODEFAULTLIB:MSVCRT"
)
