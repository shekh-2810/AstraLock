cmake_minimum_required(VERSION 3.16)
project(astralock
    VERSION 3.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# ──────────────────────────────────────────────────────────────────────────────
#  Version header — propagates PROJECT_VERSION into C++ and C automatically.
#  Avoids hardcoded version strings scattered across source files.
# ──────────────────────────────────────────────────────────────────────────────
configure_file(
    ${CMAKE_SOURCE_DIR}/version.h.in
    ${CMAKE_BINARY_DIR}/generated/facelock/version.h
    @ONLY
)
include_directories(${CMAKE_BINARY_DIR}/generated)

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(ZLIB REQUIRED)

# ──────────────────────────────────────────────────────────────────────────────
#  ONNX Runtime detection
#
#  Priority order:
#   1. CMake package config  (onnxruntime >= 1.16 apt package)
#   2. Manual tgz install    (/usr/local/include/onnxruntime/, /usr/local/lib/)
#   3. Apt package layout    (/usr/include/onnxruntime/)
#
#  Result: imported target `onnxruntime` + FACELOCK_ENABLE_ONNX=1 globally.
# ──────────────────────────────────────────────────────────────────────────────
set(FACELOCK_ONNX_FOUND FALSE)

find_package(onnxruntime QUIET)
if(onnxruntime_FOUND)
    message(STATUS "ONNX Runtime: found via CMake package config")
    set(FACELOCK_ONNX_FOUND TRUE)
    if(NOT TARGET onnxruntime)
        add_library(onnxruntime INTERFACE IMPORTED)
        target_link_libraries(onnxruntime INTERFACE onnxruntime::onnxruntime)
    endif()
endif()

if(NOT FACELOCK_ONNX_FOUND)
    find_library(ONNXRUNTIME_LIB_LOCAL onnxruntime
        PATHS /usr/local/lib NO_DEFAULT_PATH)
    find_path(ONNXRUNTIME_INC_LOCAL onnxruntime_cxx_api.h
        PATHS /usr/local/include/onnxruntime NO_DEFAULT_PATH)
    if(ONNXRUNTIME_LIB_LOCAL AND ONNXRUNTIME_INC_LOCAL)
        message(STATUS "ONNX Runtime: found via manual install (${ONNXRUNTIME_LIB_LOCAL})")
        set(FACELOCK_ONNX_FOUND TRUE)
        add_library(onnxruntime INTERFACE IMPORTED)
        target_link_libraries(onnxruntime INTERFACE ${ONNXRUNTIME_LIB_LOCAL})
        target_include_directories(onnxruntime INTERFACE
            ${ONNXRUNTIME_INC_LOCAL}/..
            ${ONNXRUNTIME_INC_LOCAL}
        )
    endif()
endif()

if(NOT FACELOCK_ONNX_FOUND)
    find_library(ONNXRUNTIME_LIB_APT onnxruntime
        PATHS /usr/lib /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
    find_path(ONNXRUNTIME_INC_APT onnxruntime_cxx_api.h
        PATHS /usr/include/onnxruntime NO_DEFAULT_PATH)
    if(ONNXRUNTIME_LIB_APT AND ONNXRUNTIME_INC_APT)
        message(STATUS "ONNX Runtime: found via apt install (${ONNXRUNTIME_LIB_APT})")
        set(FACELOCK_ONNX_FOUND TRUE)
        add_library(onnxruntime INTERFACE IMPORTED)
        target_link_libraries(onnxruntime INTERFACE ${ONNXRUNTIME_LIB_APT})
        target_include_directories(onnxruntime INTERFACE
            ${ONNXRUNTIME_INC_APT}/..
            ${ONNXRUNTIME_INC_APT}
        )
    endif()
endif()

if(NOT FACELOCK_ONNX_FOUND)
    message(FATAL_ERROR
        "ONNX Runtime not found.\n"
        "Install via:  apt install libonnxruntime-dev\n"
        "Or manually:  https://github.com/microsoft/onnxruntime/releases\n"
        "Then re-run cmake.")
endif()

add_compile_definitions(FACELOCK_ENABLE_ONNX=1)

# ──────────────────────────────────────────────────────────────────────────────
#  OpenCV — daemon now needs videoio directly (in-process camera capture).
#  The old facelock-camera-helper subprocess is eliminated in v3.
# ──────────────────────────────────────────────────────────────────────────────
find_package(OpenCV REQUIRED
    COMPONENTS core imgproc imgcodecs objdetect videoio calib3d
)

# Full OpenCV bundle — used by the daemon (includes videoio for VideoCapture)
add_library(opencv_full INTERFACE)
target_link_libraries(opencv_full INTERFACE
    opencv_core opencv_imgproc opencv_imgcodecs opencv_objdetect opencv_videoio opencv_calib3d
)

# ──────────────────────────────────────────────────────────────────────────────
#  CNpy (bundled fallback — kept for .npy model inspection tooling)
# ──────────────────────────────────────────────────────────────────────────────
find_library(CNPY_LIB cnpy)
if(CNPY_LIB)
    message(STATUS "Using system cnpy: ${CNPY_LIB}")
    set(CNPY_TARGET ${CNPY_LIB})
else()
    message(STATUS "Using bundled cnpy")
    add_subdirectory(external/cnpy)
    set(CNPY_TARGET cnpy)
    install(
        TARGETS cnpy
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

# ──────────────────────────────────────────────────────────────────────────────
#  Subdirectories
#  NOTE: helpers/ (facelock-camera-helper) is removed in v3.
#        Camera capture now happens in-process inside the daemon.
# ──────────────────────────────────────────────────────────────────────────────
add_subdirectory(daemon)
add_subdirectory(pam)

# ──────────────────────────────────────────────────────────────────────────────
#  Install misc files
# ──────────────────────────────────────────────────────────────────────────────
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_DATADIR}/facelock/models)

# Install the systemd unit from the repo (installer no longer echo-writes it)
install(
    FILES ${CMAKE_SOURCE_DIR}/systemd/facelockd.service
    DESTINATION /etc/systemd/system
)
