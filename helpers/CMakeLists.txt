add_executable(facelock-camera-helper
    facelock_camera_helper.cpp
)

target_link_libraries(facelock-camera-helper PRIVATE
    opencv_cam
)

install(TARGETS facelock-camera-helper
    RUNTIME DESTINATION /usr/lib/facelock
)
