#include <rclcpp/rclcpp.hpp>
#include <netinet/in.h>
#include <fcntl.h>
#include <tcp_tunnel/srv/add_topic.hpp>
#include <tcp_tunnel/srv/remove_topic.hpp>
#include <tcp_tunnel/srv/register_client.hpp>
#include <fstream>
#include <netinet/tcp.h>

const std::map<std::string, rclcpp::ReliabilityPolicy> RELIABILITY_POLICIES = {{"BestEffort",    rclcpp::ReliabilityPolicy::BestEffort},
                                                                               {"Reliable",      rclcpp::ReliabilityPolicy::Reliable},
                                                                               {"SystemDefault", rclcpp::ReliabilityPolicy::SystemDefault},
                                                                               {"Unknown",       rclcpp::ReliabilityPolicy::Unknown}};
const std::map<std::string, rclcpp::DurabilityPolicy> DURABILITY_POLICIES = {{"Volatile",       rclcpp::DurabilityPolicy::Volatile},
                                                                             {"TransientLocal", rclcpp::DurabilityPolicy::TransientLocal},
                                                                             {"SystemDefault",  rclcpp::DurabilityPolicy::SystemDefault},
                                                                             {"Unknown",        rclcpp::DurabilityPolicy::Unknown}};
const std::map<std::string, rclcpp::LivelinessPolicy> LIVELINESS_POLICIES = {{"Automatic",     rclcpp::LivelinessPolicy::Automatic},
                                                                             {"ManualByTopic", rclcpp::LivelinessPolicy::ManualByTopic},
                                                                             {"SystemDefault", rclcpp::LivelinessPolicy::SystemDefault},
                                                                             {"Unknown",       rclcpp::LivelinessPolicy::Unknown}};
const char CONFIRMATION_CHARACTER = '\0';

class ServiceCaller : public rclcpp::Node
{
public:
    ServiceCaller():
            rclcpp::Node("service_caller")
    {
    }
};

class TCPTunnelClient : public rclcpp::Node
{
public:
    TCPTunnelClient():
            Node("tcp_tunnel_client")
    {
        this->declare_parameter<std::string>("client_ip", "127.0.0.1");
        this->get_parameter("client_ip", clientIp);
        this->declare_parameter<std::string>("initial_topic_list_file_name", "");
        this->get_parameter("initial_topic_list_file_name", initialTopicListFileName);

        if(!initialTopicListFileName.empty())
        {
            if(initialTopicListFileName.substr(initialTopicListFileName.size() - 5, 5) != ".yaml")
            {
                throw std::runtime_error("Initial topic list file name must end with \".yaml\".");
            }

            std::this_thread::sleep_for(std::chrono::seconds(1)); // wait a bit to make sure topics are available

            std::ifstream initialTopicListFile(initialTopicListFileName);
            std::string line;
            bool startedReadingTopic = false;
            std::string topicName;
            while(std::getline(initialTopicListFile, line))
            {
                if(line.find_first_not_of(' ') != std::string::npos)
                {
                    if((!startedReadingTopic && line.substr(0, 9) != "- topic: ") || (startedReadingTopic && line.substr(0, 20) != "  server_namespace: "))
                    {
                        throw std::runtime_error("Initial topic list file does not list topics in the expected format, please refer to the README.");
                    }

                    if(!startedReadingTopic)
                    {
                        startedReadingTopic = true;
                        topicName = line.substr(9);
                    }
                    else
                    {
                        std::string serverNamespace = line.substr(20);
                        addTopic(topicName, serverNamespace);
                        startedReadingTopic = false;
                    }
                }
            }
            initialTopicListFile.close();
        }

        std::string prefix = this->get_namespace();
        if(prefix.back() != '/')
        {
            prefix += "/";
        }
        addTopicService = this->create_service<tcp_tunnel::srv::AddTopic>(prefix + "tcp_tunnel_client/add_topic",
                                                                          std::bind(&TCPTunnelClient::addTopicServiceCallback, this, std::placeholders::_1));
        removeTopicService = this->create_service<tcp_tunnel::srv::RemoveTopic>(prefix + "tcp_tunnel_client/remove_topic",
                                                                                std::bind(&TCPTunnelClient::removeTopicServiceCallback, this, std::placeholders::_1));
    }

    ~TCPTunnelClient()
    {
        for(size_t i = 0; i < threads.size(); ++i)
        {
            if(threads[i].joinable())
            {
                threads[i].join();
            }
            if(socketStatuses[i])
            {
                close(connectedSockets[i]);
                close(listeningSockets[i]);
            }
        }
    }

private:
    void addTopicServiceCallback(const std::shared_ptr<tcp_tunnel::srv::AddTopic::Request> req)
    {
        addTopic(req->topic.data, req->server_namespace.data);
    }

    void addTopic(const std::string& topicName, const std::string& serverNamespace)
    {
        // create socket
        int sockfd;
        struct sockaddr_in serv_addr;

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if(sockfd < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while creating a socket connection for topic " << topicName << ".");
            return;
        }

        if(fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while trying to set a socket flag for topic " << topicName << ".");
            close(sockfd);
            return;
        }

        bzero(&serv_addr, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(0);
        if(bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while trying to bind to socket for topic " << topicName << ".");
            close(sockfd);
            return;
        }

        socklen_t socklen = sizeof(serv_addr);
        bzero(&serv_addr, socklen);
        if(getsockname(sockfd, (struct sockaddr*)&serv_addr, &socklen) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while trying to retrieve TCP port assigned for topic " << topicName << ".");
            close(sockfd);
            return;
        }

        // call register_client service
        std::shared_ptr<tcp_tunnel::srv::RegisterClient::Request> registerClientRequest = std::make_shared<tcp_tunnel::srv::RegisterClient::Request>();
        registerClientRequest->topic.data = topicName;
        std_msgs::msg::String ip;
        ip.data = clientIp;
        registerClientRequest->client_ip = ip;
        std_msgs::msg::UInt16 port;
        port.data = ntohs(serv_addr.sin_port);
        registerClientRequest->client_port = port;

        std::string serverPrefix = serverNamespace;
        if(serverPrefix.back() != '/')
        {
            serverPrefix += "/";
        }
        std::shared_ptr<ServiceCaller> serviceCallerNode = std::make_shared<ServiceCaller>();
        rclcpp::Client<tcp_tunnel::srv::RegisterClient>::SharedPtr registerClientClient = serviceCallerNode->create_client<tcp_tunnel::srv::RegisterClient>(
                serverPrefix + "tcp_tunnel_server/register_client");
        rclcpp::detail::FutureAndRequestId registerClientFuture = registerClientClient->async_send_request(registerClientRequest);

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
            close(sockfd);
            return;
        }

        int flag = 1;
        if(setsockopt(newsockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int)) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while setting socket options for topic " << topicName << ".");
            close(newsockfd);
            close(sockfd);
            return;
        }

        if(fcntl(newsockfd, F_SETFL, O_NONBLOCK) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while trying to set a socket flag for topic " << topicName << ".");
            close(newsockfd);
            close(sockfd);
            return;
        }

        // retrieve topic info from server
        rclcpp::spin_until_future_complete(serviceCallerNode, registerClientFuture);
        tcp_tunnel::srv::RegisterClient::Response::SharedPtr registerClientResponse = registerClientFuture.get();
        if(!registerClientResponse->topic_exists.data)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Cannot add topic " << topicName << " to TCP tunnel, this topic doesn't exist.");
            close(newsockfd);
            close(sockfd);
            return;
        }
        std::string topicType = registerClientResponse->topic_type.data;
        rclcpp::QoS qos(rclcpp::KeepLast(1));
        qos.reliability(RELIABILITY_POLICIES.at(registerClientResponse->reliability_policy.data));
        qos.durability(DURABILITY_POLICIES.at(registerClientResponse->durability_policy.data));
        qos.liveliness(LIVELINESS_POLICIES.at(registerClientResponse->liveliness_policy.data));

        listeningSockets.push_back(sockfd);
        connectedSockets.push_back(newsockfd);
        socketStatuses.push_back(true);

        // create publisher
        std::string clientPrefix = this->get_namespace();
        if(clientPrefix.back() != '/')
        {
            clientPrefix += "/";
        }
        clientPrefix += "tcp_tunnel_client";
        if(topicName.front() != '/')
        {
            clientPrefix += "/";
        }
        publishers.push_back(this->create_generic_publisher(clientPrefix + topicName, topicType, qos));

        // create thread
        threads.emplace_back(&TCPTunnelClient::publishMessageLoop, this, threads.size());

        RCLCPP_INFO_STREAM(this->get_logger(), "Successfully added topic to TCP tunnel, new topic " << clientPrefix + topicName << " has been created.");
    }

    void removeTopicServiceCallback(const std::shared_ptr<tcp_tunnel::srv::RemoveTopic::Request> req)
    {
        int topicId = -1;
        for(size_t i = 0; i < publishers.size(); ++i)
        {
            if(socketStatuses[i] && publishers[i]->get_topic_name() == req->topic.data)
            {
                topicId = i;
                break;
            }
        }
        if(topicId == -1)
        {
            RCLCPP_WARN_STREAM(this->get_logger(), "Cannot remove topic " << req->topic.data << ", this topic is not in the TCP tunnel.");
            return;
        }

        pthread_cancel(threads[topicId].native_handle());
        threads[topicId].join();
        socketStatuses[topicId] = false;
        publishers[topicId].reset();
        close(connectedSockets[topicId]);
        close(listeningSockets[topicId]);

        RCLCPP_INFO_STREAM(this->get_logger(), "Successfully removed topic " << req->topic.data << " from TCP tunnel.");
    }

    void publishMessageLoop(int threadId)
    {
        rclcpp::SerializedMessage msg;
        while(rclcpp::ok())
        {
            if(!readFromSocket(threadId, &msg.get_rcl_serialized_message().buffer_length, sizeof(size_t)))
            {
                return;
            }
            msg.reserve(msg.get_rcl_serialized_message().buffer_length);
            if(!readFromSocket(threadId, msg.get_rcl_serialized_message().buffer, msg.get_rcl_serialized_message().buffer_length))
            {
                return;
            }
            if(!writeToSocket(threadId, &CONFIRMATION_CHARACTER, sizeof(char)))
            {
                return;
            }
            publishers[threadId]->publish(msg);
        }
    }

    bool readFromSocket(const int& socketId, void* buffer, const size_t& nbBytesToRead)
    {
        size_t nbBytesRead = 0;
        while(rclcpp::ok() && nbBytesRead < nbBytesToRead)
        {
            int n = read(connectedSockets[socketId], ((char*)buffer) + nbBytesRead, nbBytesToRead - nbBytesRead);
            if(n > 0)
            {
                nbBytesRead += n;
            }
            else if(n == 0 || errno == ECONNRESET)
            {
                RCLCPP_INFO_STREAM(this->get_logger(), "Connection closed by server for topic " << publishers[socketId]->get_topic_name() << ".");
                socketStatuses[socketId] = false;
                publishers[socketId].reset();
                close(connectedSockets[socketId]);
                close(listeningSockets[socketId]);
                return false;
            }
            else if(errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            else
            {
                throw std::runtime_error(
                        std::string("Error \"") + strerror(errno) + "\" occurred while reading from socket for topic " + publishers[socketId]->get_topic_name() + ".");
            }
        }
        return nbBytesRead == nbBytesToRead;
    }

    bool writeToSocket(const int& socketId, const void* buffer, const size_t& nbBytesToWrite)
    {
        size_t nbBytesWritten = 0;
        while(rclcpp::ok() && nbBytesWritten < nbBytesToWrite)
        {
            int n = send(connectedSockets[socketId], ((char*)buffer) + nbBytesWritten, nbBytesToWrite - nbBytesWritten, MSG_NOSIGNAL);
            if(n >= 0)
            {
                nbBytesWritten += n;
            }
            else if(errno == EPIPE || errno == ECONNRESET)
            {
                RCLCPP_INFO_STREAM(this->get_logger(), "Connection closed by server for topic " << publishers[socketId]->get_topic_name() << ".");
                socketStatuses[socketId] = false;
                publishers[socketId].reset();
                close(connectedSockets[socketId]);
                close(listeningSockets[socketId]);
                return false;
            }
            else
            {
                throw std::runtime_error(
                        std::string("Error \"") + strerror(errno) + "\" occurred while writing to socket for topic " + publishers[socketId]->get_topic_name() + ".");
            }
        }
        return nbBytesWritten == nbBytesToWrite;
    }

    rclcpp::Service<tcp_tunnel::srv::AddTopic>::SharedPtr addTopicService;
    rclcpp::Service<tcp_tunnel::srv::RemoveTopic>::SharedPtr removeTopicService;
    std::vector<rclcpp::GenericPublisher::SharedPtr> publishers;
    std::vector<int> listeningSockets;
    std::vector<int> connectedSockets;
    std::vector<bool> socketStatuses;
    std::vector<std::thread> threads;
    std::string clientIp;
    std::string initialTopicListFileName;
};

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<TCPTunnelClient>());
    rclcpp::shutdown();
    return 0;
}
