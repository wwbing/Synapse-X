# Shared — header-only library (protocol definitions, common types)
# Used by both Host and Client via relative path.
add_library(SynapseX_Shared INTERFACE)
target_include_directories(SynapseX_Shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(SynapseX_Shared INTERFACE cxx_std_17)
