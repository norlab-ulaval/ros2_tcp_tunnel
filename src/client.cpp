#include <rclcpp/rclcpp.hpp>
#include <netinet/in.h>
#include <fcntl.h>
#include <tcp_tunnel/srv/add_topic.hpp>
#include <tcp_tunnel/srv/remove_topic.hpp>
#include <tcp_tunnel/srv/register_client.hpp>
#include <fstream>
#include <netinet/tcp.h>
#include "semaphore.h"

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
                RCLCPP_ERROR(this->get_logger(), "Initial topic list file name must end with \".yaml\".");
                exit(1);
            }

            std::this_thread::sleep_for(std::chrono::seconds(1)); // wait a bit to make sure topics are available

            std::ifstream initialTopicListFile(initialTopicListFileName);
            if(!initialTopicListFile.good())
            {
                RCLCPP_ERROR(this->get_logger(), "Cannot open initial topic list file.");
                exit(1);
            }

            std::string line;
            std::string topicName;
            bool hasReadTopicName = false;
            std::string tunnelQueueSize;
            bool hasReadTunnelQueueSize = false;
            while(std::getline(initialTopicListFile, line))
            {
                if(line.find_first_not_of(' ') != std::string::npos)
                {
                    if((!hasReadTopicName && line.substr(0, 9) != "- topic: ") || (hasReadTopicName && !hasReadTunnelQueueSize && line.substr(0, 21) != "  tunnel_queue_size: ") ||
                       (hasReadTopicName && hasReadTunnelQueueSize && line.substr(0, 20) != "  server_namespace: "))
                    {
                        RCLCPP_ERROR(this->get_logger(), "Initial topic list file does not list topics in the expected format, please refer to the README.");
                        exit(1);
                    }

                    if(!hasReadTopicName)
                    {
                        topicName = line.substr(9);
                        hasReadTopicName = true;
                    }
                    else if(!hasReadTunnelQueueSize)
                    {
                        tunnelQueueSize = line.substr(21);
                        hasReadTunnelQueueSize = true;
                    }
                    else
                    {
                        std::string serverNamespace = line.substr(20);
                        addTopic(topicName, tunnelQueueSize, serverNamespace);
                        hasReadTopicName = false;
                        hasReadTunnelQueueSize = false;
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

        RCLCPP_INFO_STREAM(this->get_logger(),
                           "Initialization done, topics can now be added to the TCP tunnel dynamically using the " << prefix << "tcp_tunnel_client/add_topic service.");
    }

    ~TCPTunnelClient()
    {
        for(size_t i = 0; i < publishingThreads.size(); ++i)
        {
            if(publishingThreads[i].joinable())
            {
                publishingThreads[i].join();
            }
            if(confirmationThreads[i].joinable())
            {
                confirmationSemaphores[i]->wakeUp();
                confirmationThreads[i].join();
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
        addTopic(req->topic.data, req->tunnel_queue_size.data, req->server_namespace.data);
    }

    void addTopic(const std::string& topicName, const std::string& tunnelQueueSizeStr, const std::string& serverNamespace)
    {
        // convert tunnel queue size to unsigned long
        unsigned long tunnelQueueSize = 2;
        if(!tunnelQueueSizeStr.empty())
        {
            for(char character: tunnelQueueSizeStr)
            {
                if(!std::isdigit(character))
                {
                    RCLCPP_ERROR_STREAM(this->get_logger(), "Cannot add topic " << topicName << " to TCP tunnel, the passed tunnel queue size is not a valid positive integer.");
                    return;
                }
            }
            try
            {
                tunnelQueueSize = std::stoul(tunnelQueueSizeStr);

                if(tunnelQueueSize == 0)
                {
                    RCLCPP_ERROR_STREAM(this->get_logger(), "Cannot add topic " << topicName << " to TCP tunnel, the passed tunnel queue size must be greater than 0.");
                    return;
                }
            }
            catch(const std::out_of_range& exception)
            {
                tunnelQueueSize = std::numeric_limits<unsigned long>::max();
                RCLCPP_WARN_STREAM(this->get_logger(), "Passed value for tunnel queue size was clipped to " << tunnelQueueSize << " for topic " << topicName << ".");
            }
        }

        // make sure that the topic is not already in the tunnel
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
        for(size_t i = 0; i < publishers.size(); ++i)
        {
            if(socketStatuses[i] && publishers[i]->get_topic_name() == clientPrefix + topicName)
            {
                RCLCPP_WARN_STREAM(this->get_logger(), "Cannot add topic " << topicName << " to TCP tunnel, this topic is already in the tunnel.");
                return;
            }
        }

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
        registerClientRequest->tunnel_queue_size.data = tunnelQueueSize;
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
        while(std::chrono::steady_clock::now() - startTime < std::chrono::duration<float>(10) && newsockfd < 0)
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
        publishers.push_back(this->create_generic_publisher(clientPrefix + topicName, topicType, qos));

        // initialize confirmation thread
        confirmationSemaphores.emplace_back(std::make_unique<Semaphore>(0));
        confirmationThreads.emplace_back(&TCPTunnelClient::sendConfirmationLoop, this, confirmationThreads.size());

        // create thread
        publishingThreads.emplace_back(&TCPTunnelClient::publishMessageLoop, this, publishingThreads.size());

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

        pthread_cancel(publishingThreads[topicId].native_handle());
        publishingThreads[topicId].join();
        socketStatuses[topicId] = false;
        publishers[topicId].reset();
        close(connectedSockets[topicId]);
        close(listeningSockets[topicId]);
        confirmationSemaphores[topicId]->release();
        confirmationThreads[topicId].join();

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
            confirmationSemaphores[threadId]->release();
            try
            {
                publishers[threadId]->publish(msg);
            }
            catch(const rclcpp::exceptions::RCLError& error)
            {
                return;
            }
        }
    }

    bool readFromSocket(const int& socketId, void* buffer, const size_t& nbBytesToRead, const bool& isMainThread = true)
    {
        size_t nbBytesRead = 0;
        while(rclcpp::ok() && nbBytesRead < nbBytesToRead)
        {
            int n = read(connectedSockets[socketId], ((char*)buffer) + nbBytesRead, nbBytesToRead - nbBytesRead);
            if(n > 0)
            {
                nbBytesRead += n;
            }
            else if(n == 0 || errno == ECONNRESET || errno == EBADF)
            {
                if(isMainThread)
                {
                    RCLCPP_INFO_STREAM(this->get_logger(), "Connection closed by server for topic " << publishers[socketId]->get_topic_name() << ".");
                    socketStatuses[socketId] = false;
                    publishers[socketId].reset();
                    close(connectedSockets[socketId]);
                    close(listeningSockets[socketId]);
                    confirmationSemaphores[socketId]->release();
                }
                return false;
            }
            else if(errno == EWOULDBLOCK || errno == EAGAIN)
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

    void sendConfirmationLoop(int threadId)
    {
        while(rclcpp::ok())
        {
            if(!confirmationSemaphores[threadId]->acquire())
            {
                return;
            }
            if(!writeToSocket(threadId, &CONFIRMATION_CHARACTER, sizeof(char), false))
            {
                return;
            }
        }
    }

    bool writeToSocket(const int& socketId, const void* buffer, const size_t& nbBytesToWrite, const bool& isMainThread = true)
    {
        size_t nbBytesWritten = 0;
        while(rclcpp::ok() && nbBytesWritten < nbBytesToWrite)
        {
            int n = send(connectedSockets[socketId], ((char*)buffer) + nbBytesWritten, nbBytesToWrite - nbBytesWritten, MSG_NOSIGNAL);
            if(n >= 0)
            {
                nbBytesWritten += n;
            }
            else if(errno == EPIPE || errno == ECONNRESET || errno == EBADF)
            {
                if(isMainThread)
                {
                    RCLCPP_INFO_STREAM(this->get_logger(), "Connection closed by server for topic " << publishers[socketId]->get_topic_name() << ".");
                    socketStatuses[socketId] = false;
                    publishers[socketId].reset();
                    close(connectedSockets[socketId]);
                    close(listeningSockets[socketId]);
                    confirmationSemaphores[socketId]->release();
                }
                return false;
            }
            else if(errno == EWOULDBLOCK || errno == EAGAIN)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
    std::vector<std::thread> publishingThreads;
    std::vector<std::thread> confirmationThreads;
    std::vector<std::unique_ptr<Semaphore>> confirmationSemaphores;
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
