cmake_minimum_required(VERSION 3.16)
project(astralock
    VERSION 2.1
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(ZLIB REQUIRED)

# ──────────────────────────────────────────────────────────────────────────────
#  ONNX Runtime detection
#
#  Priority order:
#   1. CMake package config  (e.g. from onnxruntime >= 1.16 apt package)
#   2. Manual tgz install    (headers under /usr/local/include/onnxruntime/,
#                              lib at /usr/local/lib/libonnxruntime.so)
#   3. Apt package layout    (headers under /usr/include/onnxruntime/)
#
#  In all cases we end up with an imported target called `onnxruntime` and
#  FACELOCK_ENABLE_ONNX=1 defined on the daemon target.
# ──────────────────────────────────────────────────────────────────────────────
set(FACELOCK_ONNX_FOUND FALSE)

# 1) Try CMake package config first
find_package(onnxruntime QUIET)
if(onnxruntime_FOUND)
    message(STATUS "ONNX Runtime: found via CMake package config")
    set(FACELOCK_ONNX_FOUND TRUE)
    # The imported target may already be called onnxruntime::onnxruntime
    # Normalise to a plain alias so daemon/CMakeLists.txt can just use `onnxruntime`
    if(NOT TARGET onnxruntime)
        add_library(onnxruntime INTERFACE IMPORTED)
        target_link_libraries(onnxruntime INTERFACE onnxruntime::onnxruntime)
    endif()
endif()

# 2) Manual tgz install path  /usr/local/...
if(NOT FACELOCK_ONNX_FOUND)
    find_library(ONNXRUNTIME_LIB_LOCAL onnxruntime
        PATHS /usr/local/lib
        NO_DEFAULT_PATH)
    find_path(ONNXRUNTIME_INC_LOCAL onnxruntime_cxx_api.h
        PATHS /usr/local/include/onnxruntime
        NO_DEFAULT_PATH)

    if(ONNXRUNTIME_LIB_LOCAL AND ONNXRUNTIME_INC_LOCAL)
        message(STATUS "ONNX Runtime: found via manual install (${ONNXRUNTIME_LIB_LOCAL})")
        set(FACELOCK_ONNX_FOUND TRUE)
        add_library(onnxruntime INTERFACE IMPORTED)
        target_link_libraries(onnxruntime INTERFACE ${ONNXRUNTIME_LIB_LOCAL})
        # expose as <onnxruntime/onnxruntime_cxx_api.h>
        target_include_directories(onnxruntime INTERFACE
            ${ONNXRUNTIME_INC_LOCAL}/..   # parent → #include <onnxruntime/…>
            ${ONNXRUNTIME_INC_LOCAL}      # direct  → #include <onnxruntime_cxx_api.h>
        )
    endif()
endif()

# 3) Apt package path  /usr/include/onnxruntime/
if(NOT FACELOCK_ONNX_FOUND)
    find_library(ONNXRUNTIME_LIB_APT onnxruntime
        PATHS /usr/lib /usr/lib/x86_64-linux-gnu
        NO_DEFAULT_PATH)
    find_path(ONNXRUNTIME_INC_APT onnxruntime_cxx_api.h
        PATHS /usr/include/onnxruntime
        NO_DEFAULT_PATH)

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

# Propagate the compile definition to the daemon target (done in daemon/CMakeLists.txt
# via target_compile_definitions, but we also set it globally so nothing is missed)
add_compile_definitions(FACELOCK_ENABLE_ONNX=1)

# ──────────────────────────────────────────────────────────────────────────────
#  OpenCV
# ──────────────────────────────────────────────────────────────────────────────
find_package(OpenCV REQUIRED
    COMPONENTS core imgproc imgcodecs objdetect videoio
)

add_library(opencv_nocam INTERFACE)
target_link_libraries(opencv_nocam INTERFACE
    opencv_core opencv_imgproc opencv_imgcodecs opencv_objdetect
)

add_library(opencv_cam INTERFACE)
target_link_libraries(opencv_cam INTERFACE
    opencv_core opencv_imgproc opencv_imgcodecs opencv_objdetect opencv_videoio
)

# ──────────────────────────────────────────────────────────────────────────────
#  CNpy (bundled fallback)
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

add_subdirectory(daemon)
add_subdirectory(helpers)
add_subdirectory(pam)

install(
    DIRECTORY config/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/facelock
)

install(DIRECTORY DESTINATION ${CMAKE_INSTALL_DATADIR}/facelock/models)
