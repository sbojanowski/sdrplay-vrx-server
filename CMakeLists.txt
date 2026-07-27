cmake_minimum_required(VERSION 3.16)
project(sdrplay-vrx-server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

# Some distro-packaged yaml-cpp builds (e.g. Debian/Ubuntu libyaml-cpp-dev)
# only export the plain "yaml-cpp" target rather than the namespaced
# "yaml-cpp::yaml-cpp" alias that upstream releases >= 0.7 provide.
if(NOT TARGET yaml-cpp::yaml-cpp)
  if(TARGET yaml-cpp)
    add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
  else()
    message(FATAL_ERROR "find_package(yaml-cpp) succeeded but no yaml-cpp or yaml-cpp::yaml-cpp target was exported.")
  endif()
endif()

# The SDRplay API SDK is a proprietary vendor install, not a package-manager
# dependency - its Linux/ARM installer drops libsdrplay_api.so and
# sdrplay_api.h (plus the sdrplay_api_*.h headers it includes) under
# /usr/local/lib and /usr/local/include by default. Override with
# -DSDRPLAY_API_DIR=/path/to/sdk if yours lives elsewhere.
set(SDRPLAY_API_DIR "" CACHE PATH "Root dir containing SDRplay API's include/ and lib/ (or the .so/.h directly)")

find_path(SDRPLAY_API_INCLUDE_DIR sdrplay_api.h
  HINTS ${SDRPLAY_API_DIR} ${SDRPLAY_API_DIR}/include
  PATHS /usr/local/include /usr/include
)
find_library(SDRPLAY_API_LIB sdrplay_api
  HINTS ${SDRPLAY_API_DIR} ${SDRPLAY_API_DIR}/lib
  PATHS /usr/local/lib /usr/lib /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf
)

if(NOT SDRPLAY_API_INCLUDE_DIR OR NOT SDRPLAY_API_LIB)
  message(FATAL_ERROR
    "Could not find the SDRplay API SDK (sdrplay_api.h / libsdrplay_api). "
    "Install it from https://www.sdrplay.com/api/ or pass -DSDRPLAY_API_DIR=/path/to/sdk.")
endif()

add_executable(sdrplay-vrx-server
  src/main.cpp
  src/config.cpp
  src/sdrplay_client.cpp
  src/dsp/decimator.cpp
  src/dsp/virtual_receiver.cpp
  src/rtltcp/protocol.cpp
  src/rtltcp/server.cpp
)

target_include_directories(sdrplay-vrx-server PRIVATE
  src
  ${SDRPLAY_API_INCLUDE_DIR}
)

target_link_libraries(sdrplay-vrx-server PRIVATE
  ${SDRPLAY_API_LIB}
  yaml-cpp::yaml-cpp
  Threads::Threads
)

target_compile_options(sdrplay-vrx-server PRIVATE -O2 -Wall -Wextra)

install(TARGETS sdrplay-vrx-server RUNTIME DESTINATION bin)
