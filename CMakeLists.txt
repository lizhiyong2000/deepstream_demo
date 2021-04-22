cmake_minimum_required(VERSION 3.5 FATAL_ERROR)

project(va_worker C CXX)

find_package(PkgConfig)

pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)


set(NVDS_VERSION 5.1)

set(NVDS_DIR "/opt/nvidia/deepstream/deepstream-${NVDS_VERSION}")

set(NVDS_INCLUDE_DIR "${NVDS_DIR}/sources/includes")
set(NVDS_LIBRARY_DIR "${NVDS_DIR}/lib")

message(STATUS "deepstream dir: ${NVDS_DIR}")

message(STATUS "GSTREAMER_INCLUDE_DIRS: ${GSTREAMER_INCLUDE_DIRS}")
message(STATUS "GSTREAMER_LIBRARY_DIRS: ${GSTREAMER_LIBRARY_DIRS}")
message(STATUS "GSTREAMER_LIBRARIES: ${GSTREAMER_LIBRARIES}")



include_directories(
    include
    ${GSTREAMER_INCLUDE_DIRS}
    ${NVDS_INCLUDE_DIR}
)

link_directories(
    ${GSTREAMER_LIBRARY_DIRS}
    ${NVDS_LIBRARY_DIR}
)

link_libraries(
    nvdsgst_meta
    nvds_meta
    nvdsgst_helper
    ${GSTREAMER_LIBRARIES} 

)


  
add_executable(va_worker src/video_analytics_worker.cpp)
