cmake_minimum_required(VERSION 3.20)
project(camera_connector VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
if(OpenCV_VERSION VERSION_LESS "5.0")
  message(FATAL_ERROR "OpenCV ≥ 5.0 required (found ${OpenCV_VERSION})")
endif()
message(STATUS "OpenCV ${OpenCV_VERSION} at ${OpenCV_DIR}")

include(FetchContent)
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.17.0 GIT_SHALLOW TRUE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spdlog)
endif()

# ---------------------------------------------------------------------------
# gRPC / protobuf
# ---------------------------------------------------------------------------
set(_use_grpc_config FALSE)
if(NOT APPLE)
  find_package(gRPC CONFIG QUIET)
  if(gRPC_FOUND)
    set(_use_grpc_config TRUE)
  endif()
endif()

if(_use_grpc_config)
  set(_GRPC_GRPCPP gRPC::grpc++)
  if(TARGET gRPC::grpc_cpp_plugin)
    set(_GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
  else()
    find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  endif()
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
else()
  pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
  pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)
  if(NOT TARGET protobuf::libprotobuf)
    add_library(protobuf::libprotobuf ALIAS PkgConfig::PROTOBUF)
  endif()
  if(PROTOBUF_INCLUDE_DIRS)
    set(Protobuf_INCLUDE_DIRS ${PROTOBUF_INCLUDE_DIRS})
  endif()
  set(_GRPC_GRPCPP PkgConfig::GRPC)
  find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  find_program(_PROTOBUF_PROTOC protoc REQUIRED)
endif()

if(NOT DEFINED _PROTOBUF_PROTOC)
  if(TARGET protobuf::protoc)
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
  else()
    find_program(_PROTOBUF_PROTOC protoc REQUIRED)
  endif()
endif()

set(_PROTO_INCLUDE_PATH "")
if(DEFINED Protobuf_INCLUDE_DIRS)
  foreach(_inc ${Protobuf_INCLUDE_DIRS})
    if(EXISTS "${_inc}/google/protobuf/descriptor.proto")
      set(_PROTO_INCLUDE_PATH "${_inc}")
      break()
    endif()
  endforeach()
endif()
if(NOT _PROTO_INCLUDE_PATH)
  foreach(_cand "/opt/homebrew/include" "/usr/local/include" "/usr/include")
    if(EXISTS "${_cand}/google/protobuf/descriptor.proto")
      set(_PROTO_INCLUDE_PATH "${_cand}")
      break()
    endif()
  endforeach()
endif()
if(_PROTO_INCLUDE_PATH)
  message(STATUS "protobuf well-known types: ${_PROTO_INCLUDE_PATH}")
endif()

# ---------------------------------------------------------------------------
# Canonical protos from agentjetson/contract (no private proto/ copy)
# ---------------------------------------------------------------------------
set(AGENTJETSON_CONTRACT_GIT_TAG main CACHE STRING "agentjetson/contract ref for protos")
FetchContent_Declare(agentjetson_contract
  GIT_REPOSITORY https://github.com/agentjetson/contract.git
  GIT_TAG ${AGENTJETSON_CONTRACT_GIT_TAG}
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(agentjetson_contract)
if(NOT agentjetson_contract_POPULATED)
  FetchContent_Populate(agentjetson_contract)
endif()
set(PROTO_ROOT ${agentjetson_contract_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_FILES
  ${PROTO_ROOT}/capture/v1/frame.proto
  ${PROTO_ROOT}/capture/v1/frame_service.proto
)

set(PROTO_SRCS)
set(GRPC_SRCS)
foreach(proto ${PROTO_FILES})
  get_filename_component(proto_name ${proto} NAME_WE)
  get_filename_component(proto_dir ${proto} DIRECTORY)
  file(RELATIVE_PATH rel_dir ${PROTO_ROOT} ${proto_dir})
  set(pb_cc   ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.cc)
  set(grpc_cc ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.cc)

  set(_PROTOC_CMD
    ${_PROTOBUF_PROTOC}
    --proto_path=${PROTO_ROOT}
    --cpp_out=${PROTO_GEN_DIR}
    --grpc_out=${PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
    ${proto}
  )
  if(_PROTO_INCLUDE_PATH)
    list(INSERT _PROTOC_CMD 2 --proto_path=${_PROTO_INCLUDE_PATH})
  endif()

  add_custom_command(
    OUTPUT ${pb_cc} ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.h
           ${grpc_cc} ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.h
    COMMAND ${_PROTOC_CMD}
    DEPENDS ${proto}
    COMMENT "Generating ${proto_name}.pb.cc / .grpc.pb.cc"
    VERBATIM
  )
  list(APPEND PROTO_SRCS ${pb_cc})
  list(APPEND GRPC_SRCS ${grpc_cc})
endforeach()

add_library(camera_proto ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(camera_proto PUBLIC ${PROTO_GEN_DIR} ${Protobuf_INCLUDE_DIRS})
target_link_libraries(camera_proto PUBLIC protobuf::libprotobuf ${_GRPC_GRPCPP})

# ---------------------------------------------------------------------------
# Capture library
# ---------------------------------------------------------------------------
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

# ---------------------------------------------------------------------------
# Frame publisher (gRPC)
# ---------------------------------------------------------------------------
add_library(camera_publish
  src/publish/frame_publisher.cpp
)
target_include_directories(camera_publish PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/include
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(camera_publish PUBLIC
  camera_capture
  camera_proto
  ${OpenCV_LIBS}
  spdlog::spdlog
  ${_GRPC_GRPCPP}
  Threads::Threads
)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(camera_connector src/main.cpp)
target_link_libraries(camera_connector PRIVATE camera_publish camera_capture)

message(STATUS "camera-connector configured (local + --publish network path)")
message(STATUS "  Protos: agentjetson/contract (${AGENTJETSON_CONTRACT_GIT_TAG})")
