cmake_minimum_required(VERSION 2.8.7)

# Find required packages
find_package(Threads REQUIRED)

# Add PCAN library
find_library(PCAN_LIBRARY
    NAMES pcanbasic
    PATHS /usr/lib /usr/local/lib
)

if(NOT PCAN_LIBRARY)
    message(FATAL_ERROR "PCAN library not found. Please install PCAN-Basic")
endif()

# Add executable
add_executable(grasp main.cpp canAPI.cpp RockScissorsPaper.cpp)

# Link libraries
target_link_libraries(grasp
    ${CMAKE_THREAD_LIBS_INIT}  # For pthreads
    BHand                      # Allegro Hand library
    ${PCAN_LIBRARY}           # PCAN driver library
)

# Install targets
install(TARGETS grasp DESTINATION ${PROJECT_BINARY_DIR}/bin)
