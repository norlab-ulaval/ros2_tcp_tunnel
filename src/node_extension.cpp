#include "create_generic_subscription.hpp"
#include "node_extension.h"

NodeExtension::NodeExtension(const std::string& nodeName):
    Node(nodeName)
{
}

template<typename AllocatorT>
std::shared_ptr<rclcpp::GenericSubscription>
NodeExtension::create_generic_subscription(
        const std::string & topic_name,
        const std::string & topic_type,
        const rclcpp::QoS & qos,
        std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback,
        const rclcpp::SubscriptionOptionsWithAllocator<AllocatorT> & options)
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
