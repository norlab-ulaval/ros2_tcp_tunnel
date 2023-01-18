#include <ros/ros.h>
#include <ros_type_introspection/ros_introspection.hpp>
#include <topic_tools/shape_shifter.h>
#include <tcp_tunnel/RegisterClient.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <chrono>

std::vector<ros::Subscriber> subscribers;
std::vector<int> sockets;
std::unique_ptr<ros::NodeHandle> nodeHandle;
RosIntrospection::Parser parser;

void writeToSocket(const int& socketfd, const void* buffer, const size_t& nbBytesToWrite)
{
	size_t nbBytesWritten = 0;
	while(ros::ok() && nbBytesWritten < nbBytesToWrite)
	{
		int n = write(socketfd, ((char*)buffer) + nbBytesWritten, nbBytesToWrite - nbBytesWritten);
		if(n >= 0)
		{
			nbBytesWritten += n;
		}
	}
	if(nbBytesWritten < nbBytesToWrite)
	{
		throw std::runtime_error("An error occurred while writing to socket.");
	}
}

void subscriberCallback(const topic_tools::ShapeShifter::ConstPtr& msg, const std::string& topic_name, const int& subscriptionId)
{
	const std::string& datatype = msg->getDataType();
	const std::string& definition = msg->getMessageDefinition();
	
	parser.registerMessageDefinition(topic_name, RosIntrospection::ROSType(datatype), definition);
	std::vector<uint8_t> buffer;
	buffer.resize(msg->size());
	ros::serialization::OStream stream(buffer.data(), buffer.size());
	msg->write(stream);
	size_t size = msg->size();
	
	int n = read(sockets[subscriptionId], nullptr, 1);
	if(n != 0)
	{
		writeToSocket(sockets[subscriptionId], &size, sizeof(size_t));
		writeToSocket(sockets[subscriptionId], buffer.data(), size);
	}
	else
	{
		throw std::runtime_error("Connection closed by client.");
	}
}

bool registerClientCallback(tcp_tunnel::RegisterClient::Request& req, tcp_tunnel::RegisterClient::Response& res)
{
	std::string topicName = req.topic.data;
	
	// create socket
	int sockfd;
	struct sockaddr_in serv_addr;
	
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if(sockfd < 0)
	{
		ROS_ERROR_STREAM("Error \"" << strerror(errno) << "\" occurred while creating a socket for topic " << topicName << ".");
		return false;
	}
	fcntl(sockfd, F_SETFL, O_NONBLOCK);
	
	int flag = 1;
	if(setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int)) < 0)
	{
		ROS_ERROR_STREAM("Error \"" << strerror(errno) << "\" occurred while setting socket options for topic " << topicName << ".");
		return false;
	}
	
	// connect to client
	bzero(&serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr(req.client_ip.data.c_str());
	serv_addr.sin_port = htons(req.client_port.data);
	int n = -1;
	std::chrono::time_point<std::chrono::steady_clock> startTime = std::chrono::steady_clock::now();
	while(std::chrono::steady_clock::now() - startTime < std::chrono::duration<float>(3) && n < 0)
	{
		n = connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	}
	if(n < 0)
	{
		ROS_ERROR_STREAM("Error \"" << strerror(errno) << "\" occurred while trying to connect to " << req.client_ip.data << " on port " << req.client_port.data
									<< " for topic " << topicName << ".");
		return false;
	}
	sockets.push_back(sockfd);
	
	// create subscriber
	subscribers.push_back(nodeHandle->subscribe<topic_tools::ShapeShifter>(topicName, 1, std::bind(subscriberCallback, std::placeholders::_1, topicName,
																								   subscribers.size())));
	
	ROS_INFO_STREAM("Successfully registered client for topic " << topicName << ".");
	
	return true;
}

int main(int argc, char** argv)
{
	ros::init(argc, argv, "tcp_tunnel_server");
	nodeHandle = std::make_unique<ros::NodeHandle>();
	
	ros::ServiceServer registerClientService = nodeHandle->advertiseService("/tcp_tunnel_server/register_client", registerClientCallback);
	
	ros::MultiThreadedSpinner spinner;
	spinner.spin();
	
	for(size_t i = 0; i < sockets.size(); ++i)
	{
		close(sockets[i]);
	}
	
	return 0;
}
