#include <ros/ros.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <tcp_tunnel/AddTopic.h>
#include <tcp_tunnel/RegisterClient.h>
#include <thread>
#include <topic_tools/shape_shifter.h>
#include <rosmsg_cpp/rosmsg_cpp.h>

std::unique_ptr<ros::NodeHandle> nodeHandle;
std::string clientIp;
ros::ServiceClient registerClientClient;
std::vector<ros::Publisher> publishers;
std::vector<int> listeningSockets;
std::vector<int> connectedSockets;
std::vector<std::thread> threads;

void readFromSocket(const int& socketfd, void* buffer, const size_t& nbBytesToRead)
{
	size_t nbBytesRead = 0;
	while(ros::ok() && nbBytesRead < nbBytesToRead)
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

void publishMessageLoop(int threadId)
{
	size_t size;
	while(ros::ok())
	{
		int n = read(connectedSockets[threadId], &size, sizeof(size_t));
		if(n > 0)
		{
			readFromSocket(connectedSockets[threadId], ((char*)&size) + n, sizeof(size_t) - n);
			std::vector<uint8_t> buffer;
			buffer.resize(size);
			readFromSocket(connectedSockets[threadId], buffer.data(), size);
			ros::serialization::IStream stream(buffer.data(), buffer.size());
			topic_tools::ShapeShifter msg;
			msg.read(stream);
			publishers[threadId].publish(msg);
		}
		else if(n == 0)
		{
			throw std::runtime_error("Connection closed by server.");
		}
		else if(errno == EWOULDBLOCK)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		else
		{
			throw std::runtime_error("An error occurred while reading from socket.");
		}
	}
}

void registerAsClient(const std::string& topicName, const struct sockaddr_in& serv_addr)
{
	tcp_tunnel::RegisterClient clientRequest;
	clientRequest.request.topic.data = topicName;
	std_msgs::String ip;
	ip.data = clientIp;
	clientRequest.request.client_ip = ip;
	std_msgs::UInt16 port;
	port.data = ntohs(serv_addr.sin_port);
	clientRequest.request.client_port = port;
	registerClientClient.call(clientRequest);
}

bool addTopicCallback(tcp_tunnel::AddTopic::Request& req, tcp_tunnel::AddTopic::Response& res)
{
	ros::master::V_TopicInfo advertisedTopics;
	ros::master::getTopics(advertisedTopics);
	
	std::string topicName = req.topic.data;
	bool topicIsAdvertised = false;
	std::string topicType;
	for(size_t i = 0; i < advertisedTopics.size(); ++i)
	{
		if(advertisedTopics[i].name == topicName)
		{
			topicIsAdvertised = true;
			topicType = advertisedTopics[i].datatype;
			break;
		}
	}
	if(!topicIsAdvertised)
	{
		ROS_ERROR_STREAM("No topic named " << topicName);
		return false;
	}
	
	// create socket
	int sockfd;
	struct sockaddr_in serv_addr;
	
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if(sockfd < 0)
	{
		ROS_ERROR_STREAM("Error \"" << strerror(errno) << "\" occurred while creating a socket connection for topic " << topicName << ".");
		return false;
	}
	fcntl(sockfd, F_SETFL, O_NONBLOCK);
	
	bzero(&serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(0);
	if(bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
	{
		ROS_ERROR_STREAM("Error \"" << strerror(errno) << "\" occurred while trying to bind to socket for topic " << topicName << ".");
		return false;
	}
	listeningSockets.push_back(sockfd);
	
	socklen_t socklen = sizeof(serv_addr);
	bzero(&serv_addr, socklen);
	if(getsockname(sockfd, (struct sockaddr*)&serv_addr, &socklen) < 0)
	{
		ROS_ERROR_STREAM("Error \"" << strerror(errno) << "\" occurred while trying to retrieve TCP port assigned for topic " << topicName << ".");
		return false;
	}
	
	// call register_client service
	std::thread registerClientThread(registerAsClient, topicName, serv_addr);
	
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
		ROS_ERROR_STREAM("Error \"" << strerror(errno) << "\" occurred while accepting connection for topic " << topicName << ".");
		return false;
	}
	fcntl(newsockfd, F_SETFL, O_NONBLOCK);
	connectedSockets.push_back(newsockfd);
	
	registerClientThread.join();
	
	// create publisher
	std::string prefix = "/tcp_tunnel_client";
	if(topicName[0] != '/')
	{
		prefix += "/";
	}
	topic_tools::ShapeShifter shapeShifter;
	shapeShifter.morph(ros::message::getMD5Sum(topicType), topicType, ros::message::getFullDef(topicType), "");
	publishers.push_back(shapeShifter.advertise(*nodeHandle, prefix + topicName, 1));
	
	// create thread
	threads.emplace_back(publishMessageLoop, threads.size());
	
	ROS_INFO_STREAM("Successfully added topic to TCP tunnel, new topic " << prefix + topicName << " has been created.");
	return true;
}

int main(int argc, char** argv)
{
	ros::init(argc, argv, "tcp_tunnel_client");
	nodeHandle = std::make_unique<ros::NodeHandle>();
	ros::NodeHandle privateNodeHandle("~");
	
	privateNodeHandle.param<std::string>("client_ip", clientIp, "127.0.0.1");
	
	registerClientClient = nodeHandle->serviceClient<tcp_tunnel::RegisterClient>("/tcp_tunnel_server/register_client");
	ros::ServiceServer addTopicService = nodeHandle->advertiseService("/tcp_tunnel_client/add_topic", addTopicCallback);
	
	ros::spin();
	
	for(size_t i = 0; i < threads.size(); ++i)
	{
		close(listeningSockets[i]);
		close(connectedSockets[i]);
		threads[i].join();
	}
	
	return 0;
}
