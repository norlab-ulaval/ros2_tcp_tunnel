# ros2_tcp_tunnel
Nodes that allow reliable TCP relay of ROS topics between remote machines.

# Usage
On the publishing machine, run the server node using the following command:
```bash
rosrun tcp_tunnel server
```

On the subscribing machine, run the client node using the following command:
```bash
rosrun tcp_tunnel client _client_ip:="<client ip address>"
```

Once both nodes are running, topics can be added to the TCP tunnel dynamically using the following service call:
```bash
rosservice call /tcp_tunnel_client/add_topic "topic:
  data: '<topic name>'"
```
This will create a new topic named `/tcp_tunnel_client/<topic name>` published on the subscribing machine in which the messages of the original topic are relayed.
