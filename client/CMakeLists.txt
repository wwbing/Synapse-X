cmake_minimum_required(VERSION 3.28)

# ═══════════════════════════════════════════════════════════════
#  Synapse-X Client (副机接收端)
#
#  独立编译工程。在 Client 机器上执行：
#    cd client
#    cmake --preset ...
#
#  TODO: UDP 接收 → LZ4 解压 → TensorRT 推理 → 返回坐标
#
#  当前为占位，后续开发时补充。
# ═══════════════════════════════════════════════════════════════

project(SynapseX_Client
    VERSION 1.0.0
    DESCRIPTION "Client receiver: UDP recv → LZ4 decompress → TensorRT inference"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
