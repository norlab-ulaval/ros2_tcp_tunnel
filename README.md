# ros2_tcp_tunnel
Nodes that allow reliable TCP communication for ROS 2 between remote machines.

# Usage
On the publishing machine, run the server node using the following command:
```bash
ros2 run tcp_tunnel server
```

On the subscribing machine, run the client node using the following command:
```bash
ros2 run tcp_tunnel client --ros-args -p client_ip:="<client ip address>"
```

Once both nodes are running, topics can be added to the TCP tunnel dynamically using the following service call:
```bash
ros2 service call /add_topic tcp_tunnel/srv/AddTopic "topic:
  data: '<topic name>'"
```
