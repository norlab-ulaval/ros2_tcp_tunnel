cmake_minimum_required(VERSION 3.8)
project(tcp_tunnel)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_BUILD_TYPE "Release")

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "srv/RegisterClient.srv"
  "srv/AddTopic.srv"
  "srv/RemoveTopic.srv"
  DEPENDENCIES std_msgs
)

include_directories(include)

add_executable(server src/server.cpp src/semaphore.cpp)
ament_target_dependencies(server
  rclcpp
  std_msgs
)

add_library(server_component src/server_component.cpp src/semaphore.cpp)
ament_target_dependencies(server_component
  rclcpp
  std_msgs
  rclcpp_components
)
rclcpp_components_register_node(
        server_component
        PLUGIN "TCPTunnelServer"
        EXECUTABLE server_component_exec
)

add_executable(client src/client.cpp src/semaphore.cpp)
ament_target_dependencies(client
  rclcpp
  std_msgs
)

rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")
target_link_libraries(client "${cpp_typesupport_target}")
target_link_libraries(server "${cpp_typesupport_target}")
target_link_libraries(server_component "${cpp_typesupport_target}")
ament_export_dependencies(rosidl_default_runtime)

install(TARGETS client server
  DESTINATION lib/${PROJECT_NAME}
)

ament_export_targets(export_server_component)
install(TARGETS server_component
        EXPORT export_server_component
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
)

ament_package()
