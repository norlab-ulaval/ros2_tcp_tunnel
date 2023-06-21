cmake_minimum_required(VERSION 3.8)
project(tcp_tunnel)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_BUILD_TYPE "Release")

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(ament_index_cpp REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "srv/RegisterClient.srv"
  "srv/AddTopic.srv"
  "srv/RemoveTopic.srv"
  DEPENDENCIES std_msgs
)

include_directories(include)

add_executable(server src/server.cpp src/semaphore.cpp src/generic_subscription.cpp src/typesupport_helpers.cpp)
ament_target_dependencies(server
  rclcpp
  std_msgs
  ament_index_cpp
)
rosidl_target_interfaces(server
  ${PROJECT_NAME} "rosidl_typesupport_cpp"
)

add_executable(client src/client.cpp src/semaphore.cpp src/generic_publisher.cpp src/typesupport_helpers.cpp)
ament_target_dependencies(client
  rclcpp
  std_msgs
  ament_index_cpp
)
rosidl_target_interfaces(client
  ${PROJECT_NAME} "rosidl_typesupport_cpp"
)

ament_export_dependencies(rosidl_default_runtime)

install(TARGETS client server
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
