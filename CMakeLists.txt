cmake_minimum_required(VERSION 3.10)

project(iccp VERSION 0.1)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# ICCP protocol library (header-only + compiled helpers)
add_library(iccp-lib STATIC
  src/protocol/protocol.cpp
)

target_include_directories(iccp-lib PUBLIC src)

# --- executables ---

# Manager server
add_executable(iccp-manager
  src/manager/manager.cpp
)
target_link_libraries(iccp-manager iccp-lib)

# Sensor client
add_executable(iccp-sensor
  src/sensor/sensor.cpp
)
target_link_libraries(iccp-sensor iccp-lib)

# Actuator client
add_executable(iccp-actuator
  src/actuator/actuator.cpp
)
target_link_libraries(iccp-actuator iccp-lib)

# Client application
add_executable(iccp-client
  src/client/client.cpp
)
target_link_libraries(iccp-client iccp-lib)
