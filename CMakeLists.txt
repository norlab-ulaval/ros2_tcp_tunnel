cmake_minimum_required(VERSION 2.8.3)
project(tcp_tunnel)

set(CMAKE_BUILD_TYPE "Release")

find_package(catkin REQUIRED COMPONENTS
  roscpp
  std_msgs
  ros_type_introspection
  topic_tools
  rosmsg_cpp
  message_generation
)

add_service_files(
  FILES
  RegisterClient.srv
  AddTopic.srv
)

generate_messages(
  DEPENDENCIES
  std_msgs
)

catkin_package(
  CATKIN_DEPENDS message_runtime std_msgs
)

include_directories(
  ${catkin_INCLUDE_DIRS}
)

add_executable(server src/server.cpp)
add_dependencies(server ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})
target_link_libraries(server
  ${catkin_LIBRARIES}
)

add_executable(client src/client.cpp)
add_dependencies(client ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})
target_link_libraries(client
  ${catkin_LIBRARIES}
)
