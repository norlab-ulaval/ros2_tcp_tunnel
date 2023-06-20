#ifndef TCP_TUNNEL_NODE_EXTENSION_HPP
#define TCP_TUNNEL_NODE_EXTENSION_HPP

#include <rclcpp/node.hpp>
#include "generic_subscription.hpp"
#include "create_generic_subscription.hpp"

class NodeExtension : public rclcpp::Node
{
public:
    NodeExtension(const std::string& nodeName):
            Node(nodeName)
    {
    }

    template<typename AllocatorT = std::allocator<void>>
    std::shared_ptr<rclcpp::GenericSubscription> create_generic_subscription(
            const std::string & topic_name,
            const std::string & topic_type,
            const rclcpp::QoS & qos,
            std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback,
            const rclcpp::SubscriptionOptionsWithAllocator<AllocatorT> & options = (
                    rclcpp::SubscriptionOptionsWithAllocator<AllocatorT>()
            )
    )
    {
        return rclcpp::create_generic_subscription(
                get_node_topics_interface(),
                rclcpp::extend_name_with_sub_namespace(topic_name, this->get_sub_namespace()),
                topic_type,
                qos,
                std::move(callback),
                options
        );
    }
};

#endif
