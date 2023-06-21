#include <rclcpp/rclcpp.hpp>
#include <tcp_tunnel/srv/register_client.hpp>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include "semaphore.h"
#include "node_extension.hpp"

const std::map<rmw_qos_reliability_policy_t, std::string> RELIABILITY_POLICIES = {{rmw_qos_reliability_policy_t::RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT,"BestEffort"},
                                                                               {rmw_qos_reliability_policy_t::RMW_QOS_POLICY_RELIABILITY_RELIABLE,"Reliable"},
                                                                               {rmw_qos_reliability_policy_t::RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT,"SystemDefault"},
                                                                               {rmw_qos_reliability_policy_t::RMW_QOS_POLICY_RELIABILITY_UNKNOWN,"Unknown"}};
const std::map<rmw_qos_durability_policy_t, std::string> DURABILITY_POLICIES = {{rmw_qos_durability_policy_t::RMW_QOS_POLICY_DURABILITY_VOLATILE,"Volatile"},
                                                                             {rmw_qos_durability_policy_t::RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL,"TransientLocal"},
                                                                             {rmw_qos_durability_policy_t::RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT,"SystemDefault"},
                                                                             {rmw_qos_durability_policy_t::RMW_QOS_POLICY_DURABILITY_UNKNOWN,"Unknown"}};
const std::map<rmw_qos_liveliness_policy_t, std::string> LIVELINESS_POLICIES = {{rmw_qos_liveliness_policy_t::RMW_QOS_POLICY_LIVELINESS_AUTOMATIC,"Automatic"},
                                                                             {rmw_qos_liveliness_policy_t::RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC,"ManualByTopic"},
                                                                             {rmw_qos_liveliness_policy_t::RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT,"SystemDefault"},
                                                                             {rmw_qos_liveliness_policy_t::RMW_QOS_POLICY_LIVELINESS_UNKNOWN,"Unknown"}};

class TCPTunnelServer : public NodeExtension
{
public:
    TCPTunnelServer():
            NodeExtension("tcp_tunnel_server")
    {
        std::string prefix = this->get_namespace();
        if(prefix.back() != '/')
        {
            prefix += "/";
        }
        registerClientService = this->create_service<tcp_tunnel::srv::RegisterClient>(prefix + "tcp_tunnel_server/register_client",
                                                                                      std::bind(&TCPTunnelServer::registerClientCallback, this, std::placeholders::_1,
                                                                                                std::placeholders::_2));
    }

    ~TCPTunnelServer()
    {
        for(size_t i = 0; i < sockets.size(); ++i)
        {
            if(confirmationThreads[i].joinable())
            {
                confirmationSemaphores[i]->wakeUp();
                confirmationThreads[i].join();
            }
            if(socketStatuses[i])
            {
                close(sockets[i]);
            }
        }
    }

private:
    void registerClientCallback(const std::shared_ptr<tcp_tunnel::srv::RegisterClient::Request> req, std::shared_ptr<tcp_tunnel::srv::RegisterClient::Response> res)
    {
        std::string topicName = req->topic.data;

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
            close(sockfd);
            return;
        }

        if(fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Error \"" << strerror(errno) << "\" occurred while trying to set a socket flag for topic " << topicName << ".");
            close(sockfd);
            return;
        }

        // connect to client
        int n = -1;
        bzero(&serv_addr, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = inet_addr(req->client_ip.data.c_str());
        serv_addr.sin_port = htons(req->client_port.data);
        std::chrono::time_point<std::chrono::steady_clock> startTime = std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now() - startTime < std::chrono::duration<float>(3) && n < 0)
        {
            n = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        }
        if(n < 0)
        {
            RCLCPP_ERROR_STREAM(this->get_logger(),
                                "Error \"" << strerror(errno) << "\" occurred while trying to connect to " << req->client_ip.data << " on port " << req->client_port.data
                                           << " for topic " << topicName << ".");
            close(sockfd);
            return;
        }

        // fetch topic info
        if(this->get_topic_names_and_types().count(topicName) == 0 || this->get_publishers_info_by_topic(topicName).empty())
        {
            RCLCPP_ERROR_STREAM(this->get_logger(), "Cannot add topic " << topicName << " to TCP tunnel, this topic doesn't exist.");
            close(sockfd);
            res->topic_exists.data = false;
            return;
        }
        std::string topicType = this->get_topic_names_and_types()[topicName][0];
        rclcpp::QoS qos = this->get_publishers_info_by_topic(topicName)[0].qos_profile();

        sockets.push_back(sockfd);
        socketStatuses.push_back(true);

        // initialize confirmation thread
        confirmationSemaphores.emplace_back(std::make_unique<Semaphore>(req->tunnel_queue_size.data));
        confirmationThreads.emplace_back(&TCPTunnelServer::receiveConfirmationLoop, this, confirmationThreads.size());

        // create subscription
        subscriptions.push_back(this->create_generic_subscription(topicName, topicType, qos.keep_last(1),
                                                                  std::bind(&TCPTunnelServer::subscriptionCallback, this, std::placeholders::_1, subscriptions.size())));

        // return topic info to client
        res->topic_exists.data = true;
        res->topic_type.data = topicType;
        res->reliability_policy.data = RELIABILITY_POLICIES.at(qos.get_rmw_qos_profile().reliability);
        res->durability_policy.data = DURABILITY_POLICIES.at(qos.get_rmw_qos_profile().durability);
        res->liveliness_policy.data = LIVELINESS_POLICIES.at(qos.get_rmw_qos_profile().liveliness);

        RCLCPP_INFO_STREAM(this->get_logger(), "Successfully registered client for topic " << topicName << ".");
    }

    void subscriptionCallback(std::shared_ptr<rclcpp::SerializedMessage> msg, const int& subscriptionId)
    {
        if(!confirmationSemaphores[subscriptionId]->tryAcquire())
        {
            return;
        }
        if(!writeToSocket(subscriptionId, &msg->get_rcl_serialized_message().buffer_length, sizeof(size_t)))
        {
            return;
        }
        if(!writeToSocket(subscriptionId, msg->get_rcl_serialized_message().buffer, msg->get_rcl_serialized_message().buffer_length))
        {
            return;
        }
    }

    bool writeToSocket(const int& socketId, const void* buffer, const size_t& nbBytesToWrite, const bool& isMainThread = true)
    {
        size_t nbBytesWritten = 0;
        while(rclcpp::ok() && nbBytesWritten < nbBytesToWrite)
        {
            int n = send(sockets[socketId], ((char*)buffer) + nbBytesWritten, nbBytesToWrite - nbBytesWritten, MSG_NOSIGNAL);
            if(n >= 0)
            {
                nbBytesWritten += n;
            }
            else if(errno == EPIPE || errno == ECONNRESET || errno == EBADF)
            {
                if(isMainThread)
                {
                    RCLCPP_INFO_STREAM(this->get_logger(), "Connection closed by client for topic " << subscriptions[socketId]->get_topic_name() << ".");
                    socketStatuses[socketId] = false;
                    subscriptions[socketId].reset();
                    close(sockets[socketId]);
                }
                else
                {
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
                        std::string("Error \"") + strerror(errno) + "\" occurred while writing to socket for topic " + subscriptions[socketId]->get_topic_name() + ".");
            }
        }
        return nbBytesWritten == nbBytesToWrite;
    }

    void receiveConfirmationLoop(int threadId)
    {
        while(rclcpp::ok())
        {
            if(!readFromSocket(threadId, &confirmationBuffer, sizeof(char), false))
            {
                return;
            }
            confirmationSemaphores[threadId]->release();
        }
    }

    bool readFromSocket(const int& socketId, void* buffer, const size_t& nbBytesToRead, const bool& isMainThread = true)
    {
        size_t nbBytesRead = 0;
        while(rclcpp::ok() && nbBytesRead < nbBytesToRead)
        {
            int n = read(sockets[socketId], ((char*)buffer) + nbBytesRead, nbBytesToRead - nbBytesRead);
            if(n > 0)
            {
                nbBytesRead += n;
            }
            else if(n == 0 || errno == ECONNRESET || errno == EBADF)
            {
                if(isMainThread)
                {
                    RCLCPP_INFO_STREAM(this->get_logger(), "Connection closed by client for topic " << subscriptions[socketId]->get_topic_name() << ".");
                    socketStatuses[socketId] = false;
                    subscriptions[socketId].reset();
                    close(sockets[socketId]);
                }
                else
                {
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
                        std::string("Error \"") + strerror(errno) + "\" occurred while reading from socket for topic " + subscriptions[socketId]->get_topic_name() + ".");
            }
        }
        return nbBytesRead == nbBytesToRead;
    }

    rclcpp::Service<tcp_tunnel::srv::RegisterClient>::SharedPtr registerClientService;
    std::vector<rclcpp::GenericSubscription::SharedPtr> subscriptions;
    std::vector<int> sockets;
    std::vector<bool> socketStatuses;
    std::vector<std::thread> confirmationThreads;
    std::vector<std::unique_ptr<Semaphore>> confirmationSemaphores;
    char confirmationBuffer;
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
