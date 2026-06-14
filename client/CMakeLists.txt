cmake_minimum_required(VERSION 3.28)

# ═══════════════════════════════════════════════════════════════
#  Synapse-X Client (副机接收端)
#
#  Standalone build. On the Client machine:
#    cd client
#    cmake --preset windows-x64
#    cmake --build build_x64 --config RelWithDebInfo
#
#  Pipeline: UDP recv → LZ4 decompress → [TensorRT inference]
#
#  Directory layout:
#    include/             — public headers
#    src/                 — implementation + main
# ═══════════════════════════════════════════════════════════════

project(SynapseX_Client
    VERSION 1.0.0
    DESCRIPTION "Client receiver: UDP recv → LZ4 decompress → TensorRT inference"
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

# ═══════════════════════════════════════════════════════════════
#  LZ4 (compiled directly from thirdparty source)
# ═══════════════════════════════════════════════════════════════
set(LZ4_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/lz4-1.10.0/lib")
add_library(lz4_static STATIC
    "${LZ4_SRC_DIR}/lz4.c"
    "${LZ4_SRC_DIR}/lz4.h"
)
target_include_directories(lz4_static PUBLIC "${LZ4_SRC_DIR}")
if(MSVC)
    target_compile_options(lz4_static PRIVATE /W0)
endif()

# ═══════════════════════════════════════════════════════════════
#  Shared protocol headers (header-only)
# ═══════════════════════════════════════════════════════════════
add_library(SynapseX_Shared INTERFACE)
target_include_directories(SynapseX_Shared INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)
target_compile_features(SynapseX_Shared INTERFACE cxx_std_17)

# ═══════════════════════════════════════════════════════════════
#  CUDA 13.1 — 寻址逻辑（暂不编译 .cu，仅为后续 TensorRT 推理打地基）
# ═══════════════════════════════════════════════════════════════
# 优先使用 CMake 原生 find_package；若失败则回退到手动路径。
set(SX_CUDA_FOUND FALSE)

# 尝试自动发现 CUDA（CMake 3.17+ 的 CUDAToolkit 方式）
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    message(STATUS "[SynapseX Client] CUDA Toolkit found via find_package:")
    message(STATUS "  Version:        ${CUDAToolkit_VERSION}")
    message(STATUS "  Include dirs:   ${CUDAToolkit_INCLUDE_DIRS}")
    message(STATUS "  Toolkit dir:    ${CUDAToolkit_TARGET_DIR}")
    set(SX_CUDA_FOUND TRUE)
    set(SX_CUDA_INCLUDE_DIRS ${CUDAToolkit_INCLUDE_DIRS})
    set(SX_CUDA_LIB_DIR     "${CUDAToolkit_TARGET_DIR}/lib/x64")
else()
    # 手动指定 CUDA 13.1 安装路径
    set(CUDA_BASE_PATH "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1")
    if(EXISTS "${CUDA_BASE_PATH}/include/cuda.h")
        message(STATUS "[SynapseX Client] CUDA 13.1 found at manual path: ${CUDA_BASE_PATH}")
        set(SX_CUDA_FOUND TRUE)
        set(SX_CUDA_INCLUDE_DIRS "${CUDA_BASE_PATH}/include")
        set(SX_CUDA_LIB_DIR     "${CUDA_BASE_PATH}/lib/x64")
    else()
        message(WARNING "[SynapseX Client] CUDA 13.1 NOT found. "
                        "TensorRT inference will be disabled. "
                        "Expected at: ${CUDA_BASE_PATH}")
    endif()
endif()

# ═══════════════════════════════════════════════════════════════
#  TensorRT 10.16.1.11 — 手动寻址
# ═══════════════════════════════════════════════════════════════
set(SX_TENSORRT_FOUND FALSE)
set(TRT_BASE_PATH "C:/Program Files/NVIDIA/TensorRT-10.16.1.11.Windows.amd64.cuda-13.2/TensorRT-10.16.1.11")

if(EXISTS "${TRT_BASE_PATH}/include/NvInfer.h")
    message(STATUS "[SynapseX Client] TensorRT found at: ${TRT_BASE_PATH}")
    set(SX_TENSORRT_FOUND TRUE)
    set(SX_TRT_INCLUDE_DIRS "${TRT_BASE_PATH}/include")

    if(EXISTS "${TRT_BASE_PATH}/lib")
        set(SX_TRT_LIB_DIR "${TRT_BASE_PATH}/lib")
    endif()

    if(SX_CUDA_FOUND)
        message(STATUS "[SynapseX Client] CUDA + TensorRT 10.16: both available. "
                       "Enabling inference module.")
    endif()
else()
    message(WARNING "[SynapseX Client] TensorRT NOT found at ${TRT_BASE_PATH}. "
                    "Inference target will be skipped.")
endif()

# ═══════════════════════════════════════════════════════════════
#  TensorRT 推理模块
# ═══════════════════════════════════════════════════════════════
if(SX_CUDA_FOUND AND SX_TENSORRT_FOUND)
    add_library(SynapseX_Inference STATIC
        src/TrtInference.cpp
    )
    target_include_directories(SynapseX_Inference PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
        ${SX_CUDA_INCLUDE_DIRS}
        ${SX_TRT_INCLUDE_DIRS}
    )
    target_compile_definitions(SynapseX_Inference PRIVATE NOMINMAX)
    target_link_libraries(SynapseX_Inference PUBLIC
        ${SX_TRT_LIB_DIR}/nvinfer_10.lib
        ${SX_TRT_LIB_DIR}/nvinfer_plugin_10.lib
        ${SX_CUDA_LIB_DIR}/cudart.lib
        ${SX_CUDA_LIB_DIR}/cuda.lib
    )
    set(SX_HAS_INFERENCE TRUE)
else()
    set(SX_HAS_INFERENCE FALSE)
endif()

# ═══════════════════════════════════════════════════════════════
#  Client module sources
# ═══════════════════════════════════════════════════════════════
set(CLIENT_SOURCES
    src/UdpReceiver.cpp
    src/UdpReplySender.cpp
    src/TrtInference.cpp
    src/main.cpp
)

set(CLIENT_LIBS
    SynapseX_Shared
    lz4_static
    ws2_32
)

# ── Main executable ────────────────────────────────────
add_executable(SynapseX_Client ${CLIENT_SOURCES})
target_include_directories(SynapseX_Client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)
target_compile_definitions(SynapseX_Client PRIVATE NOMINMAX)
target_link_libraries(SynapseX_Client PRIVATE ${CLIENT_LIBS})

# 链接 CUDA + TensorRT 推理模块
if(SX_HAS_INFERENCE)
    target_include_directories(SynapseX_Client PRIVATE
        ${SX_CUDA_INCLUDE_DIRS}
        ${SX_TRT_INCLUDE_DIRS}
    )
    target_link_libraries(SynapseX_Client PRIVATE
        SynapseX_Inference
        ${SX_TRT_LIB_DIR}/nvinfer_10.lib
        ${SX_TRT_LIB_DIR}/nvinfer_plugin_10.lib
        ${SX_CUDA_LIB_DIR}/cudart.lib
        ${SX_CUDA_LIB_DIR}/cuda.lib
    )
endif()

# ── Summary ────────────────────────────────────────────
message(STATUS "")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "  SynapseX Client — Build Configuration")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "  LZ4:           ${LZ4_SRC_DIR}")
message(STATUS "  Shared:        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include")
message(STATUS "  CUDA found:    ${SX_CUDA_FOUND}")
message(STATUS "  TensorRT found: ${SX_TENSORRT_FOUND}")
message(STATUS "  Inference:     ${SX_HAS_INFERENCE}")
message(STATUS "  Winsock2:      ws2_32")
if(SX_CUDA_FOUND)
    message(STATUS "  CUDA:          ${SX_CUDA_INCLUDE_DIRS}")
endif()
if(SX_TENSORRT_FOUND)
    message(STATUS "  TRT:           ${SX_TRT_INCLUDE_DIRS}")
endif()
message(STATUS "══════════════════════════════════════════════")
message(STATUS "")
