#include <rclcpp/rclcpp.hpp>
#include <tcp_tunnel/srv/register_client.hpp>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

class TCPTunnelServer : public rclcpp::Node
{
public:
    TCPTunnelServer():
            Node("tcp_tunnel_server")
    {
        std::string prefix = this->get_namespace();
        if(prefix.back() != '/')
        {
            prefix += "/";
        }
        registerClientService = this->create_service<tcp_tunnel::srv::RegisterClient>(prefix + "tcp_tunnel_server/register_client",
                                                                                      std::bind(&TCPTunnelServer::registerClientCallback, this, std::placeholders::_1));
    }

    ~TCPTunnelServer()
    {
        for(size_t i = 0; i < sockets.size(); ++i)
        {
            if(socketStatuses[i])
            {
                close(sockets[i]);
            }
        }
    }

    void registerClientCallback(const std::shared_ptr<tcp_tunnel::srv::RegisterClient::Request> req)
    {
        std::string topicName = req->topic.data;
        if(this->get_topic_names_and_types().count(topicName) == 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "No topic named " << topicName);
            return;
        }
        std::string topicType = this->get_topic_names_and_types()[topicName][0];

        // create socket
        int sockfd;
        struct sockaddr_in serv_addr;

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if(sockfd < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while creating a socket for topic " << topicName << ".");
            return;
        }

        int flag = 1;
        if(setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int)) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while setting socket options for topic " << topicName << ".");
            return;
        }

        // connect to client
        bzero(&serv_addr, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = inet_addr(req->client_ip.data.c_str());
        serv_addr.sin_port = htons(req->client_port.data);
        if(connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(),
                                "Error \"" << strerror(errno) << "\" occurred while trying to connect to " << req->client_ip.data << " on port " << req->client_port.data
                                           << " for topic " << topicName << ".");
            return;
        }
        sockets.push_back(sockfd);
        socketStatuses.push_back(true);

        // create subscription
        rclcpp::QoS qos = rclcpp::QoS(rclcpp::KeepLast(1));
        subscriptions.push_back(
                this->create_generic_subscription(topicName, topicType, qos, std::bind(&TCPTunnelServer::subscriptionCallback, this, std::placeholders::_1, subscriptions.size())));

        RCLCPP_INFO_STREAM(this->get_logger(), "Successfully registered client for topic " << topicName << ".");
    }

    void subscriptionCallback(std::shared_ptr<rclcpp::SerializedMessage> msg, const int& subscriptionId)
    {
        writeToSocket(subscriptionId, &msg->get_rcl_serialized_message().buffer_length, sizeof(size_t));
        writeToSocket(subscriptionId, msg->get_rcl_serialized_message().buffer, msg->get_rcl_serialized_message().buffer_length);
    }

    void writeToSocket(const int& socketId, const void* buffer, const size_t& nbBytesToWrite)
    {
        if(socketStatuses[socketId])
        {
            size_t nbBytesWritten = 0;
            while(rclcpp::ok() && nbBytesWritten < nbBytesToWrite)
            {
                int n = send(sockets[socketId], ((char*)buffer) + nbBytesWritten, nbBytesToWrite - nbBytesWritten, MSG_NOSIGNAL);
                if(n >= 0)
                {
                    nbBytesWritten += n;
                }
                else if(errno == EPIPE)
                {
                    RCLCPP_INFO_STREAM(this->get_logger(), "Connection closed by client for topic " << subscriptions[socketId]->get_topic_name() << ".");
                    socketStatuses[socketId] = false;
                    subscriptions[socketId].reset();
                    close(sockets[socketId]);
                    return;
                }
            }
            if(nbBytesWritten < nbBytesToWrite)
            {
                throw std::runtime_error("An error occurred while writing to socket.");
            }
        }
    }

private:
    rclcpp::Service<tcp_tunnel::srv::RegisterClient>::SharedPtr registerClientService;
    std::vector<rclcpp::GenericSubscription::SharedPtr> subscriptions;
    std::vector<int> sockets;
    std::vector<bool> socketStatuses;
};

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
    std::shared_ptr<rclcpp::Node> node = std::make_shared<TCPTunnelServer>();
    rclcpp::executors::MultiThreadedExecutor executor;
    executor.add_node(node);
    executor.spin();
    rclcpp::shutdown();
    return 0;
}
