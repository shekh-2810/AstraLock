add_executable(facelockd
    src/daemon.cpp
    src/ipc_server.cpp
    src/main.cpp
    src/lbph_wrapper.cpp
    src/lbph_embedder.cpp
    src/face_aligner.cpp
    src/onnx_wrapper.cpp
    src/storage.cpp
)

target_include_directories(facelockd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# CNpy headers (bundled only)
if(TARGET cnpy)
    target_include_directories(facelockd PRIVATE
        ${CMAKE_SOURCE_DIR}/external/cnpy
    )
endif()

# -------- Core libraries --------
target_link_libraries(facelockd PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    ZLIB::ZLIB
    opencv_nocam
    ${CNPY_TARGET}
)

# -------- ONNX Runtime (optional, safe) --------
find_path(ONNX_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATHS /usr/include/onnxruntime
)

find_library(ONNX_LIB
    NAMES onnxruntime
)

if(ONNX_INCLUDE_DIR AND ONNX_LIB)
    message(STATUS "ONNX Runtime found")

    target_include_directories(facelockd PRIVATE ${ONNX_INCLUDE_DIR})
    target_link_libraries(facelockd PRIVATE ${ONNX_LIB})
    target_compile_definitions(facelockd PRIVATE FACELOCK_ENABLE_ONNX)
else()
    message(WARNING "ONNX Runtime not found — building without ONNX")
endif()

# -------- Runtime linking (self-contained) --------
set_target_properties(facelockd PROPERTIES
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
)

install(TARGETS facelockd
    RUNTIME DESTINATION /usr/lib/facelock
)