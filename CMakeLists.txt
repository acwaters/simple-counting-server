cmake_minimum_required(VERSION 3.19)

project(counting-server)

file(GLOB SOURCES  "*.cpp")
file(GLOB INCLUDES "*.hpp")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_FLAGS "-Wall -Wextra -pedantic -Wno-missing-field-initializers")

add_executable(${PROJECT_NAME} ${SOURCES} ${INCLUDES})

install(TARGETS counting-server DESTINATION bin)
install(FILES counting-server.service DESTINATION /etc/systemd/system/)
install(CODE "execute_process(COMMAND systemctl enable counting-server)")
