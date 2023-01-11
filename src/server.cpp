#include <rclcpp/rclcpp.hpp>
#include <tcp_tunnel/srv/add_client.hpp>
#include <netinet/in.h>
#include <arpa/inet.h>

class TCPTunnelServer : public rclcpp::Node
{
public:
    TCPTunnelServer():
            Node("tcp_tunnel_server")
    {

        addClientService = this->create_service<tcp_tunnel::srv::AddClient>("add_client",
                                                                            std::bind(&TCPTunnelServer::addClientCallback, this, std::placeholders::_1, std::placeholders::_2));
    }

    ~TCPTunnelServer()
    {
        for(int i = 0; i < sockets.size(); ++i)
        {
            close(sockets[i]);
        }
    }

    void addClientCallback(const std::shared_ptr<tcp_tunnel::srv::AddClient::Request> req,
                           std::shared_ptr<tcp_tunnel::srv::AddClient::Response> res)
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
            RCLCPP_ERROR_STREAM(this->get_logger(), "An error occurred while creating a socket for topic " << topicName << ".");
            return;
        }
        RCLCPP_INFO(this->get_logger(), "Created socket");

        bzero((char*)&serv_addr, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = inet_addr(req->client_ip.data.c_str());
        serv_addr.sin_port = htons(req->client_port.data);
        if(connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(),
                                "An error occurred while trying to connect to " << req->client_ip.data << " on port " << req->client_port.data << " for topic " << topicName
                                                                                << ".");
            return;
        }
        sockets.push_back(sockfd);
        RCLCPP_INFO(this->get_logger(), "Connected");

        // create subscription
        rclcpp::QoS qos = rclcpp::QoS(rclcpp::KeepLast(10));
        subscriptions.push_back(
                this->create_generic_subscription(topicName, topicType, qos, std::bind(&TCPTunnelServer::subscriptionCallback, this, std::placeholders::_1, subscriptions.size())));
    }

    void subscriptionCallback(std::shared_ptr<rclcpp::SerializedMessage> msg, const int& subscriptionId)
    {
        size_t dataSize = msg->get_rcl_serialized_message().buffer_capacity;
        const void* sizeBuf = (const void*)&dataSize;

        int n = write(sockets[subscriptionId], sizeBuf, sizeof(size_t));
        if(n < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "An error occurred while writing to socket for topic " << subscriptions[subscriptionId]->get_topic_name() << ".");
            return;
        }

        void* dataBuf = malloc(dataSize);
        memcpy(dataBuf, msg->get_rcl_serialized_message().buffer, dataSize);

        n = write(sockets[subscriptionId], dataBuf, dataSize);
        if(n < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "An error occurred while writing to socket for topic " << subscriptions[subscriptionId]->get_topic_name() << ".");
            return;
        }

        free(dataBuf);
    }

private:
    rclcpp::Service<tcp_tunnel::srv::AddClient>::SharedPtr addClientService;
    std::vector<rclcpp::GenericSubscription::SharedPtr> subscriptions;
    std::vector<int> sockets;
};

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
//    rclcpp::executors::MultiThreadedExecutor executor;
//    executor.add_node(std::make_shared<TCPTunnelServer>());
//    executor.spin();
    rclcpp::spin(std::make_shared<TCPTunnelServer>());
    rclcpp::shutdown();
    return 0;
}
