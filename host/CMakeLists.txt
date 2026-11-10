cmake_minimum_required(VERSION 3.16)
project(agent_indicator_host VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(agentind_core STATIC
  src/json.cpp
  src/platform.cpp
  src/tracker.cpp
  src/composer.cpp
  src/config.cpp
  src/serial.cpp
  src/feed.cpp)
target_include_directories(agentind_core PUBLIC src)
target_compile_options(agentind_core PRIVATE -Wall -Wextra)

add_executable(agent-indicator
  src/main.cpp src/hook.cpp src/daemon.cpp src/install.cpp)
target_link_libraries(agent-indicator PRIVATE agentind_core)
target_compile_options(agent-indicator PRIVATE -Wall -Wextra)
target_compile_definitions(agent-indicator PRIVATE AGENT_INDICATOR_VERSION="${PROJECT_VERSION}")

install(TARGETS agent-indicator RUNTIME DESTINATION bin)

option(AGENTIND_TESTS "Build unit tests" ON)
if(AGENTIND_TESTS)
  enable_testing()
  add_executable(agentind_tests tests/tests.cpp)
  target_link_libraries(agentind_tests PRIVATE agentind_core)
  add_test(NAME agentind_tests COMMAND agentind_tests)
endif()
