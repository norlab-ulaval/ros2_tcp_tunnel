# ros2_tcp_tunnel
Nodes that allow reliable TCP relay of ROS 2 topics between remote machines.

## Basic Usage
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
  data: '<topic name>'
server_namespace:
  data: '<server namespace>'"
```
If the server node is located in the global namespace (default), the `server_namespace` field can be left empty or can be set to '/'.
This will create a new topic named `/tcp_tunnel_client/<topic name>` published on the subscribing machine in which the messages of the original topic are relayed.

## Advanced Usage
### Providing a list of topics on startup of the client node
It is possible to provide a YAML file listing all the topics to add to the TCP tunnel when starting the client node.
For instance, if the user wanted to add the topics `/foo` and `/bar` automatically on startup of the client node, a YAML file with the following content should be created:
```yaml
- topic: /foo
  server_namespace: /

- topic: /bar
  server_namespace: /
```
If a server node is located in the global namespace (default), its `server_namespace` field can be left blank or can be set to '/'.
This topic list can then be passed to the client node on startup using the following command:
```bash
ros2 run tcp_tunnel client --ros-args -p client_ip:="<client ip address>" -p initial_topic_list_file_name:="<yaml file name>"
```

### Running multiple client nodes simultaneously
In order to run multiple client nodes simultaneously, each client node must be located in its own namespace.
This can be achieved by running the client nodes with the following command:
```bash
ros2 run tcp_tunnel client --ros-args -p client_ip:="<client ip address>" -r __ns:="<client namespace>"
```
Make sure to provide a namespace starting with '/'.
It is then possible to call the client nodes `add_topic` services in their respective namespace:
```bash
ros2 service call <client namespace>/tcp_tunnel_client/add_topic tcp_tunnel/srv/AddTopic "topic:
  data: '<topic name>'
server_namespace:
  data: '<server namespace>'"
```

### Running multiple server nodes simultaneously
Similarly, to run multiple server nodes simultaneously, each server node must be located in its own namespace.
This can be achieved by running the server nodes with the following command:
```bash
ros2 run tcp_tunnel server --ros-args -r __ns:="<server namespace>"
```
Make sure to provide a namespace starting with '/'.
Then, when adding a topic to the TCP tunnel, the proper server namespace must be passed in the service call:
```bash
ros2 service call /tcp_tunnel_client/add_topic tcp_tunnel/srv/AddTopic "topic:
  data: '<topic name>'
server_namespace:
  data: '<server namespace>'"
```
