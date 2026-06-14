cmake_minimum_required(VERSION 3.28)
project(SynapseX
    VERSION 1.0.0
    DESCRIPTION "Ultra-low latency visual inference — mono-repo root"
    LANGUAGES CXX
)

# ═══════════════════════════════════════════════════════════════
#  此文件仅为 mono-repo 根目录提供 IDE 上下文。
#
#  Host 和 Client 是两台物理隔离的机器，各自独立编译：
#   · Host:  cd host   && cmake --preset windows-x64
#   · Client: cd client && cmake --preset ...
#
#  共享头文件 (shared/) 通过相对路径 ../shared/ 被双方引用。
# ═══════════════════════════════════════════════════════════════

# 仅提供 shared 头文件库的 IDE 索引（不参与实际编译产出）
add_subdirectory(shared)
