#include <rclcpp/rclcpp.hpp>
#include <netinet/in.h>
#include <fcntl.h>
#include <tcp_tunnel/srv/add_topic.hpp>
#include <tcp_tunnel/srv/register_client.hpp>

class TCPTunnelClient : public rclcpp::Node
{
public:
    TCPTunnelClient():
            Node("tcp_tunnel_client")
    {
        this->declare_parameter<std::string>("client_ip", "127.0.0.1");
        this->get_parameter("client_ip", clientIp);

        registerClientClient = this->create_client<tcp_tunnel::srv::RegisterClient>("/tcp_tunnel_server/register_client");
        addTopicService = this->create_service<tcp_tunnel::srv::AddTopic>("/tcp_tunnel_client/add_topic",
                                                                          std::bind(&TCPTunnelClient::addTopicCallback, this, std::placeholders::_1));
    }

    ~TCPTunnelClient()
    {
        for(size_t i = 0; i < threads.size(); ++i)
        {
            close(listeningSockets[i]);
            close(connectedSockets[i]);
            threads[i].join();
        }
    }

    void addTopicCallback(const std::shared_ptr<tcp_tunnel::srv::AddTopic::Request> req)
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
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while creating a socket connection for topic " << topicName << ".");
            return;
        }
        fcntl(sockfd, F_SETFL, O_NONBLOCK);

        bzero(&serv_addr, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(0);
        if(bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while trying to bind to socket for topic " << topicName << ".");
            return;
        }
        listeningSockets.push_back(sockfd);

        socklen_t socklen = sizeof(serv_addr);
        bzero(&serv_addr, socklen);
        if(getsockname(sockfd, (struct sockaddr*)&serv_addr, &socklen) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while trying to retrieve TCP port assigned for topic " << topicName << ".");
            return;
        }

        // call register_client service
        std::shared_ptr<tcp_tunnel::srv::RegisterClient::Request> clientRequest = std::make_shared<tcp_tunnel::srv::RegisterClient::Request>();
        clientRequest->topic = req->topic;
        std_msgs::msg::String ip;
        ip.data = clientIp;
        clientRequest->client_ip = ip;
        std_msgs::msg::UInt16 port;
        port.data = ntohs(serv_addr.sin_port);
        clientRequest->client_port = port;
        registerClientClient->async_send_request(clientRequest);

        // connect to server
        int newsockfd = -1;
        struct sockaddr_in cli_addr;
        socklen_t clilen;

        listen(sockfd, 5);
        clilen = sizeof(cli_addr);
        std::chrono::time_point<std::chrono::steady_clock> startTime = std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now() - startTime < std::chrono::duration<float>(3) && newsockfd < 0)
        {
            newsockfd = accept(sockfd, (struct sockaddr*)&cli_addr, &clilen);
        }
        if(newsockfd < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while accepting connection for topic " << topicName << ".");
            return;
        }
        fcntl(newsockfd, F_SETFL, O_NONBLOCK);
        connectedSockets.push_back(newsockfd);

        // create publisher
        rclcpp::QoS qos = rclcpp::QoS(rclcpp::KeepLast(10));
        std::string prefix = "/tcp_tunnel_client";
        if(topicName[0] != '/')
        {
            prefix += "/";
        }
        publishers.push_back(this->create_generic_publisher(prefix + topicName, topicType, qos));

        // create thread
        threads.emplace_back(&TCPTunnelClient::publishMessageLoop, this, threads.size());

        RCLCPP_INFO_STREAM(this->get_logger(), "Successfully added topic to TCP tunnel, new topic " << prefix + topicName << " has been created.");
    }

    void publishMessageLoop(int threadId)
    {
        rclcpp::SerializedMessage msg;
        while(rclcpp::ok())
        {
            int n = read(connectedSockets[threadId], &msg.get_rcl_serialized_message().buffer_length, sizeof(size_t));
            if(n >= 0)
            {
                readFromSocket(connectedSockets[threadId], ((char*)&msg.get_rcl_serialized_message().buffer_length) + n, sizeof(size_t) - n);
                msg.reserve(msg.get_rcl_serialized_message().buffer_length);
                readFromSocket(connectedSockets[threadId], msg.get_rcl_serialized_message().buffer, msg.get_rcl_serialized_message().buffer_length);
                publishers[threadId]->publish(msg);
            }
            else if(errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            else
            {
                RCLCPP_ERROR_STREAM(this->get_logger(),
                                    "Error \"" << strerror(errno) << "\" occurred while reading from socket for topic " << publishers[threadId]->get_topic_name() << ".");
                return;
            }
        }
    }

    void readFromSocket(const int& socketfd, void* buffer, const size_t& nbBytesToRead)
    {
        size_t nbBytesRead = 0;
        std::chrono::time_point<std::chrono::steady_clock> startTime = std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now() - startTime < std::chrono::duration<float>(3) && nbBytesRead < nbBytesToRead)
        {
            int n = read(socketfd, ((char*)buffer) + nbBytesRead, nbBytesToRead - nbBytesRead);
            if(n >= 0)
            {
                nbBytesRead += n;
            }
        }
        if(nbBytesRead < nbBytesToRead)
        {
            throw std::runtime_error("An error occurred while reading from socket.");
        }
    }

private:
    rclcpp::Client<tcp_tunnel::srv::RegisterClient>::SharedPtr registerClientClient;
    rclcpp::Service<tcp_tunnel::srv::AddTopic>::SharedPtr addTopicService;
    std::vector<rclcpp::GenericPublisher::SharedPtr> publishers;
    std::vector<int> listeningSockets;
    std::vector<int> connectedSockets;
    std::vector<std::thread> threads;
    std::string clientIp;
};

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<TCPTunnelClient>());
    rclcpp::shutdown();
    return 0;
}
