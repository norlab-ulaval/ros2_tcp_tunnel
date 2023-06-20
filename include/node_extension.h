#ifndef TCP_TUNNEL_NODE_EXTENSION_H
#define TCP_TUNNEL_NODE_EXTENSION_H

#include <rclcpp/node.hpp>
#include "generic_subscription.hpp"

class NodeExtension : public rclcpp::Node
{
public:
    NodeExtension(const std::string& nodeName);

    template<typename AllocatorT = std::allocator<void>>
    std::shared_ptr<rclcpp::GenericSubscription> create_generic_subscription(
            const std::string & topic_name,
            const std::string & topic_type,
            const rclcpp::QoS & qos,
            std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback,
            const rclcpp::SubscriptionOptionsWithAllocator<AllocatorT> & options = (
                    rclcpp::SubscriptionOptionsWithAllocator<AllocatorT>()
            )
    );
};

#endif //TCP_TUNNEL_NODE_EXTENSION_H
