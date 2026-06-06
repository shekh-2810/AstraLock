# ============================================================
#  AstraLock v3 — daemon build
#
#  Removed vs v2.1:
#    - face_aligner.cpp / face_aligner.h  (dead code)
#    - helpers/ facelock-camera-helper     (replaced by camera.cpp)
#
#  Added vs v2.1:
#    - camera.cpp            in-process camera + liveness pipeline
#    - libsystemd            for sd_notify(READY=1)
#    - opencv_videoio        daemon owns VideoCapture now
# ============================================================

set(DAEMON_SOURCES
    src/main.cpp
    src/daemon.cpp
    src/camera.cpp
    src/storage.cpp
    src/ipc_server.cpp
    src/onnx_wrapper.cpp
)

add_executable(facelockd ${DAEMON_SOURCES})

target_include_directories(facelockd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # Generated version.h lives here:
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(facelockd PRIVATE
    onnxruntime
    opencv_full          # core + imgproc + imgcodecs + objdetect + videoio
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ${CNPY_TARGET}
    ZLIB::ZLIB
    Threads::Threads
    systemd              # sd_notify
)

install(
    TARGETS facelockd
    RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR}   # /usr/sbin/facelockd
)