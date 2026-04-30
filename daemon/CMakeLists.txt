add_executable(facelockd
    src/daemon.cpp
    src/ipc_server.cpp
    src/main.cpp
    src/face_aligner.cpp
    src/onnx_wrapper.cpp
    src/storage.cpp
)

target_include_directories(facelockd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(TARGET cnpy)
    target_include_directories(facelockd PRIVATE
        ${CMAKE_SOURCE_DIR}/external/cnpy
    )
endif()

# Explicitly set FACELOCK_ENABLE_ONNX on this target — belt-and-suspenders
# alongside the global add_compile_definitions() in the root CMakeLists.
target_compile_definitions(facelockd PRIVATE FACELOCK_ENABLE_ONNX=1)

target_link_libraries(facelockd PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    ZLIB::ZLIB
    opencv_nocam
    onnxruntime
    ${CNPY_TARGET}
)

set_target_properties(facelockd PROPERTIES
    BUILD_RPATH  "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
)

install(TARGETS facelockd
    RUNTIME DESTINATION /usr/lib/facelock
)
