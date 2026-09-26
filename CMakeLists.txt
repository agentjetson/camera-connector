cmake_minimum_required(VERSION 3.20)
project(camera_connector VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)

include(FetchContent)
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1 GIT_SHALLOW TRUE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spdlog)
endif()

add_library(camera_capture
  src/capture/capture.cpp
)
target_include_directories(camera_capture PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/include
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(camera_capture PUBLIC
  ${OpenCV_LIBS}
  spdlog::spdlog
  Threads::Threads
)

add_executable(camera_connector src/main.cpp)
target_link_libraries(camera_connector PRIVATE camera_capture)

message(STATUS "camera-connector configured")
