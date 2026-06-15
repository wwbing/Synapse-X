cmake_minimum_required(VERSION 3.28)

# ═══════════════════════════════════════════════════════════════
#  Synapse-X Host (Host sender)
#
#  Standalone build. On the Host machine:
#    cd host
#    cmake --preset windows-x64
#    cmake --build build_x64 --config RelWithDebInfo
#
#  Directory layout:
#    include/SynapseX/   — public headers
#    src/                — implementation + main
#    test/               — test programs
# ═══════════════════════════════════════════════════════════════

project(SynapseX_Host
    VERSION 1.0.0
    DESCRIPTION "Host sender: DXGI capture -> LZ4 compress -> UDP send"
    LANGUAGES C CXX
)

# ── C++17 ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Export compile commands (for clangd / IDE) ─────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Windows SDK ────────────────────────────────────────
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINIMAX)
endif()

# ── MSVC compile options ───────────────────────────────
if(MSVC)
    add_compile_options(
        /utf-8
        /W3
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
    )
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        $<$<CONFIG:Release>:NDEBUG>
    )
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Gy /Oi")
endif()

# ── LZ4 (compiled directly from thirdparty source) ─────
set(LZ4_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/lz4-1.10.0/lib")
add_library(lz4_static STATIC
    "${LZ4_SRC_DIR}/lz4.c"
    "${LZ4_SRC_DIR}/lz4.h"
)
target_include_directories(lz4_static PUBLIC "${LZ4_SRC_DIR}")
if(MSVC)
    target_compile_options(lz4_static PRIVATE /W0)
endif()

# ── Shared protocol headers (header-only) ──────────────
add_library(SynapseX_Shared INTERFACE)
target_include_directories(SynapseX_Shared INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)
target_compile_features(SynapseX_Shared INTERFACE cxx_std_17)

# ── Host module sources ────────────────────────────────
set(HOST_SOURCES
    src/DxgiCapturer.cpp
    src/Lz4Compressor.cpp
    src/UdpSender.cpp
    src/UdpReplyReceiver.cpp
    src/MouseController.cpp
    src/HttpTuner.cpp
    src/main.cpp
)

set(HOST_LIBS
    SynapseX_Shared
    lz4_static
    d3d11
    dxgi
    ws2_32
    winmm
)

# ── Main executable ────────────────────────────────────
add_executable(SynapseX_Host ${HOST_SOURCES})
target_include_directories(SynapseX_Host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/cpp-httplib-0.47.0
)
target_link_libraries(SynapseX_Host PRIVATE ${HOST_LIBS})

# Copy ddll64.dll to build output so MouseController can find it
add_custom_command(TARGET SynapseX_Host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/mousedll/ddll64.dll"
        "$<TARGET_FILE_DIR:SynapseX_Host>/ddll64.dll"
    COMMENT "Copying ddll64.dll to output directory"
)

# Copy web/ to build output so HttpTuner can find index.html
add_custom_command(TARGET SynapseX_Host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/web"
        "$<TARGET_FILE_DIR:SynapseX_Host>/web"
    COMMENT "Copying web/ to output directory"
)

# ── BMP capture + LZ4 compression test ─────────────────
add_executable(SynapseX_Host_TestBmp
    test/test_bmp.cpp
    src/DxgiCapturer.cpp
    src/Lz4Compressor.cpp
)
target_include_directories(SynapseX_Host_TestBmp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)
target_link_libraries(SynapseX_Host_TestBmp PRIVATE ${HOST_LIBS})
