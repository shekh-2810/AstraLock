cmake_minimum_required(VERSION 3.10)
project(pam_facelock C)

set(CMAKE_C_STANDARD 99)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PAM REQUIRED pam)

add_library(pam_facelock SHARED pam_facelock.c)

target_include_directories(pam_facelock PRIVATE ${PAM_INCLUDE_DIRS})
target_link_libraries(pam_facelock ${PAM_LIBRARIES})

set_target_properties(pam_facelock PROPERTIES
    PREFIX ""
    OUTPUT_NAME pam_facelock
)
