// Copyright 2014 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCP_TUNNEL_NODE_EXTENSION_HPP
#define TCP_TUNNEL_NODE_EXTENSION_HPP

#include <rclcpp/node.hpp>
#include "generic_subscription.hpp"
#include "create_generic_subscription.hpp"
#include "generic_publisher.hpp"
#include "create_generic_publisher.hpp"

class NodeExtension : public rclcpp::Node
{
public:
    NodeExtension(const std::string& nodeName):
            Node(nodeName)
    {
    }

    template<typename AllocatorT = std::allocator<void>>
    std::shared_ptr<rclcpp::GenericSubscription> create_generic_subscription(
            const std::string& topic_name,
            const std::string& topic_type,
            const rclcpp::QoS& qos,
            std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> callback,
            const rclcpp::SubscriptionOptionsWithAllocator<AllocatorT>& options = (
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

    template<typename AllocatorT = std::allocator<void>>
    std::shared_ptr<rclcpp::GenericPublisher> create_generic_publisher(
            const std::string& topic_name,
            const std::string& topic_type,
            const rclcpp::QoS& qos,
            const rclcpp::PublisherOptionsWithAllocator<AllocatorT>& options = (
                    rclcpp::PublisherOptionsWithAllocator<AllocatorT>()
            )
    )
    {
        return rclcpp::create_generic_publisher(
                get_node_topics_interface(),
                rclcpp::extend_name_with_sub_namespace(topic_name, this->get_sub_namespace()),
                topic_type,
                qos,
                options
        );
    }
};

#endif
