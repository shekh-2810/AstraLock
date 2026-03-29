cmake_minimum_required(VERSION 3.16)
project(astralock
    VERSION 2.0
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

# ------------------ ONNX Runtime ------------------
find_package(onnxruntime QUIET)
if(NOT onnxruntime_FOUND)
    find_library(ONNXRUNTIME_LIB onnxruntime REQUIRED)
    find_path(ONNXRUNTIME_INCLUDE onnxruntime_cxx_api.h
        PATHS /usr/include/onnxruntime /usr/local/include/onnxruntime REQUIRED)
    add_library(onnxruntime INTERFACE IMPORTED)
    target_link_libraries(onnxruntime INTERFACE ${ONNXRUNTIME_LIB})
    target_include_directories(onnxruntime INTERFACE ${ONNXRUNTIME_INCLUDE})
    message(STATUS "ONNX Runtime found")
else()
    message(STATUS "ONNX Runtime found")
endif()

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

# ------------------ CNpy ------------------
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

# ------------------ Install shared assets ------------------
install(
    DIRECTORY config/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/facelock
)

install(DIRECTORY DESTINATION ${CMAKE_INSTALL_DATADIR}/facelock/models)
