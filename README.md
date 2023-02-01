# ros2_tcp_tunnel
Nodes that allow reliable TCP relay of ROS 2 topics between remote machines.

# Basic Usage
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
ros2 service call /tcp_tunnel_client/add_topic tcp_tunnel/srv/AddTopic "topic:
  data: '<topic name>'"
```
This will create a new topic named `/tcp_tunnel_client/<topic name>` published on the subscribing machine in which the messages of the original topic are relayed.

# Advanced Usage
It is also possible to provide a YAML file listing all the topics to add to the TCP tunnel when starting the client node.
For instance, if the user wanted to add the topics `/foo` and `/bar` automatically on startup of the client node, a YAML file with the following content should be created:
```yaml
- /foo
- /bar
```
This topic list can then be passed to the client node on startup using the following command:
```bash
ros2 run tcp_tunnel client --ros-args -p client_ip:="<client ip address>" -p initial_topic_list_file_name:="<yaml file name>"
```
