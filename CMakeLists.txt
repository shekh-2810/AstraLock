cmake_minimum_required(VERSION 3.16)

project(astralock
    VERSION 0.1
    LANGUAGES C CXX
)

# ------------------ Global settings ------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# ------------------ Core dependencies ------------------

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(ZLIB REQUIRED)

# ------------------ OpenCV (single discovery) ------------------

find_package(OpenCV REQUIRED
    COMPONENTS
        core
        imgproc
        imgcodecs
        objdetect
        videoio
)

# ------------------ OpenCV interface targets ------------------

# No camera (daemon)
add_library(opencv_nocam INTERFACE)
target_link_libraries(opencv_nocam INTERFACE
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    opencv_objdetect
)

# Camera enabled (helper)
add_library(opencv_cam INTERFACE)
target_link_libraries(opencv_cam INTERFACE
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    opencv_objdetect
    opencv_videoio
)

# ------------------ CNpy ------------------

# Prefer system cnpy if present
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

# ------------------ Subprojects ------------------

add_subdirectory(daemon)
add_subdirectory(helpers)
add_subdirectory(pam)

# ------------------ Install shared assets ------------------

install(
    DIRECTORY config/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/facelock
)

install(
    FILES
        scripts/train_lbph.py
        scripts/make_emb_bin_from_raw.py
    DESTINATION ${CMAKE_INSTALL_DATADIR}/facelock
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
)
