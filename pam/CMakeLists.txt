cmake_minimum_required(VERSION 3.16)
project(pam_facelock C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PAM REQUIRED pam)

add_library(pam_facelock SHARED pam_facelock.c)

target_include_directories(pam_facelock PRIVATE ${PAM_INCLUDE_DIRS})
target_link_libraries(pam_facelock PRIVATE ${PAM_LIBRARIES})

# Strip the lib prefix — PAM expects pam_facelock.so not libpam_facelock.so
set_target_properties(pam_facelock PROPERTIES
    PREFIX ""
    OUTPUT_NAME "pam_facelock"
)

# Install to the PAM security module directory.
# Adjust path for non-Debian distros (Fedora: /lib64/security/).
if(NOT DEFINED PAM_SECURITY_DIR)
    set(PAM_SECURITY_DIR "/lib/x86_64-linux-gnu/security")
endif()

install(
    TARGETS pam_facelock
    LIBRARY DESTINATION ${PAM_SECURITY_DIR}
)