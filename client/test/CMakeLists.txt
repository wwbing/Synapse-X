cmake_minimum_required(VERSION 3.28)
project(SynapseX_Test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CUDA
find_package(CUDAToolkit REQUIRED)
set(CUDA_ROOT "${CUDAToolkit_TARGET_DIR}")
set(CUDA_INC   "${CUDA_ROOT}/include")
set(CUDA_LIB   "${CUDA_ROOT}/lib/x64")

# TensorRT
set(TRT_ROOT "C:/Program Files/NVIDIA/TensorRT-10.16.1.11.Windows.amd64.cuda-13.2/TensorRT-10.16.1.11")
set(TRT_INC   "${TRT_ROOT}/include")
set(TRT_LIB   "${TRT_ROOT}/lib")

# Inference sources (from parent client project)
set(CLIENT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(CLIENT_INC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
set(SHARED_INC "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/include")

add_executable(test_infer
    src/main.cpp
    ${CLIENT_SRC}/TrtInference.cpp
    ${CLIENT_SRC}/CudaPreprocess.cpp
)

target_include_directories(test_infer PRIVATE
    include
    ${CLIENT_INC}
    ${SHARED_INC}
    ${CUDA_INC}
    ${TRT_INC}
)

target_compile_definitions(test_infer PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(test_infer PRIVATE /utf-8 /W3)
endif()

target_link_libraries(test_infer
    ${TRT_LIB}/nvinfer_10.lib
    ${TRT_LIB}/nvinfer_plugin_10.lib
    ${CUDA_LIB}/cudart.lib
    ${CUDA_LIB}/cuda.lib
    ${CUDA_LIB}/nvrtc.lib
    windowscodecs    # WIC
    ole32            # COM
)
