#pragma once

#include <dv-processing/core/core.hpp>
#include <dv-processing/core/frame.hpp>
#include <dv-processing/data/frame_base.hpp>
#include <dv-processing/exception/exception.hpp>

#define DV_ROS_MSGS(type) type##_<boost::container::allocator<void>>

#include <dv_ros2_msgs/msg/event_array.hpp>
#include <dv_ros2_msgs/msg/event_packet.hpp>
#include <dv_ros2_msgs/msg/trigger.hpp>

#include <boost/bind/bind.hpp>
#include <boost/container/allocator.hpp>
#include <opencv2/core.hpp>

#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/image_encodings.hpp>
#include <rclcpp/rclcpp.hpp>

namespace dv_ros2_msgs 
{

namespace _detail {
[[nodiscard]] inline int32_t imageTypeFromEncoding(const std::string &encoding) 
{
	if (sensor_msgs::image_encodings::isBayer(encoding)) 
    {
		throw dv::exceptions::RuntimeError("Bayer image encoding is not supported for conversion!");
	}

	const int channels = sensor_msgs::image_encodings::numChannels(encoding);
	const int depth    = sensor_msgs::image_encodings::bitDepth(encoding);
	switch (depth) 
    {
		case 8: {
			return CV_MAKETYPE(CV_8U, channels);
			break;
		}
		case 16: {
			return CV_MAKETYPE(CV_16U, channels);
			break;
		}
		default:
			throw dv::exceptions::InvalidArgument<int>("Unsupported image bit depth", depth);
	}
}
} // namespace _detail

/// @brief Converts UNIX microsecond timestamp into rclcpp::Time format.
/// @param timestamp DV format UNIX microsecond timestamp
/// @return ROS2 timestamp
[[nodiscard]] inline rclcpp::Time toRosTime(const int64_t timestamp) 
{
	return {static_cast<uint32_t>(timestamp / 1'000'000), static_cast<uint32_t>((timestamp % 1'000'000) * 1'000)};
}

/// @brief Convert rclcpp::Time time into UNIX microsecond timestamp
/// @param timestamp ROS2 timestamp
/// @return DV format UNIX microsecond timestamp
[[nodiscard]] inline int64_t toDvTime(const rclcpp::Time &timestamp) 
{
	return (static_cast<int64_t>(timestamp.seconds()) * 1'000'000) + (timestamp.nanoseconds() / 1'000);
}

/// @brief Convert OpenCV image into ROS image message. Supports only single channel 8-bit, three channel 8-bit BGR images,
///        and continous and non-continous memory.
///        Performs deep data copy.
/// @param image OpenCV image
/// @return ROS2 image message (sensor_msgs::msg::Image)
/// @throw dv::exceptions::RuntimeError if image data layout is not supported
[[nodiscard]] inline sensor_msgs::msg::Image toRosImageMessage(const cv::Mat &image) 
{
	sensor_msgs::msg::Image msg;

	msg.height = image.rows;
	msg.width  = image.cols;

	if (image.empty()) 
    {
		return msg;
	}

	switch (image.type()) 
    {
		case CV_8UC1:
			msg.encoding = sensor_msgs::image_encodings::MONO8;
			break;
		case CV_8UC3:
			msg.encoding = sensor_msgs::image_encodings::BGR8;
			break;
		default:
			throw dv::exceptions::RuntimeError("Received unsupported image type");
	}

	msg.is_bigendian  = false;
	msg.step          = msg.width * image.elemSize();
	const size_t size = msg.step * msg.height;
	msg.data.resize(size);

	if (image.isContinuous()) 
    {
		memcpy((char *) (&msg.data[0]), image.data, size);
	}
	else 
    {
		auto ros_data_ptr  = (uchar *) (&msg.data[0]);
		uchar *cv_data_ptr = image.data;
		for (int i = 0; i < image.rows; ++i) 
        {
			memcpy(ros_data_ptr, cv_data_ptr, msg.step);
			ros_data_ptr += msg.step;
			cv_data_ptr += image.step;
		}
	}
	return msg;
}

///@brief Converts dv::Frame into sensor_msgs::msg::Image.
///@param frame DV Frame containing an image.
///@return ROS2 image (sensor_msgs::msg::Image)
///@throws RuntimeError If image data layout is not supported
[[nodiscard]] inline sensor_msgs::msg::Image frameToRosImageMessage(const dv::Frame &frame) 
{
	sensor_msgs::msg::Image imageMessage = toRosImageMessage(frame.image);
	imageMessage.header.stamp = toRosTime(frame.timestamp);
	return imageMessage;
}

/// @brief Convert dv::IMU into sensor_msgs::Imu
/// @param imu DV IMU measurement
/// @return ROS Imu message
[[nodiscard]] inline sensor_msgs::msg::Imu toRosImuMessage(const dv::IMU &imu) 
{
	sensor_msgs::msg::Imu imuMessage;
	imuMessage.header.stamp = toRosTime(imu.timestamp);

    constexpr float pi = 3.14159265358979323846f;
	constexpr float deg2rad = pi / 180.0f;
	constexpr float earthG  = 9.81007f;

	imuMessage.angular_velocity.x    = imu.gyroscopeX * deg2rad;
	imuMessage.angular_velocity.y    = imu.gyroscopeY * deg2rad;
	imuMessage.angular_velocity.z    = imu.gyroscopeZ * deg2rad;
	imuMessage.linear_acceleration.x = imu.accelerometerX * earthG;
	imuMessage.linear_acceleration.y = imu.accelerometerY * earthG;
	imuMessage.linear_acceleration.z = imu.accelerometerZ * earthG;

	return imuMessage;
}

/// @brief Convert dv::Trigger into dv_ros2_msgs::msg::Trigger
/// @param trigger DV Trigger
/// @return ROS2 Trigger message
[[nodiscard]] inline dv_ros2_msgs::msg::Trigger toRosTriggerMessage(const dv::Trigger &trigger) 
{
	dv_ros2_msgs::msg::Trigger msg;
	msg.timestamp = toRosTime(trigger.timestamp);
	msg.type      = static_cast<int8_t>(trigger.type);
	return msg;
}


/// @brief Convert dv::EventStore into dv_ros2_msgs::msg::EventArray
/// @param events DV EventStore
/// @param resolution Resolution of the sensor
/// @return ROS2 EventArray message
///[[nodiscard]] inline dv_ros2_msgs::msg::EventArray toRosEventsMessage(const dv::EventStore &events, const cv::Size &resolution) 
[[nodiscard]] inline dv_ros2_msgs::msg::EventPacket toRosEventsMessage(const dv::EventStore &events, const cv::Size &resolution) 
{
	//dv_ros2_msgs::msg::EventArray msg;
	dv_ros2_msgs::msg::EventPacket msg;
	rclcpp::Time time = toRosTime(events.getLowestTime());

	int64_t secInMicro = static_cast<int64_t>(time.seconds()) * 1'000'000;
	msg.header.stamp   = rclcpp::Time(static_cast<double>(events.getHighestTime()) * 1e-6);
	msg.events.reserve(events.size());
	for (const auto &event : events) 
    {
		int64_t time_diff = event.timestamp() - secInMicro;
		if (time_diff < 1'000'000) 
        {
			// We are in the same second, we only need to update the nano-second part
			time = {static_cast<uint32_t>(time.seconds()), static_cast<uint32_t>(time_diff * 1'000)};
		}
		else 
        {
			time       = toRosTime(event.timestamp());
			secInMicro = static_cast<int64_t>(time.seconds()) * 1'000'000;
		}
		auto &e       = msg.events.emplace_back();
		e.x           = event.x();
		e.y           = event.y();
		e.polarity    = event.polarity();
		e.ts 		  = time;
	}

	msg.width  = resolution.width;
	msg.height = resolution.height;
	return msg;
}

/// @brief Convert an array message into an event store.
/// @param message Event array message
/// @return DV Event store
//[[nodiscard]] inline dv::EventStore toEventStore(const dv_ros2_msgs::msg::EventArray &message)
[[nodiscard]] inline dv::EventStore toEventStore(const dv_ros2_msgs::msg::EventPacket &message)
{
	if (message.events.empty())
    {
		return {};
	}
	uint32_t seconds                             = message.events.front().ts.sec;
	int64_t timestamp                            = static_cast<int64_t>(seconds) * 1'000'000;
	std::shared_ptr<dv::EventPacket> eventPacket = std::make_shared<dv::EventPacket>();
	eventPacket->elements.reserve(message.events.size());
	for (const auto &event : message.events)
    {
		if (event.ts.sec != seconds)
        {
			seconds   = event.ts.sec;
			timestamp = static_cast<int64_t>(seconds) * 1'000'000;
		}
		const int64_t eventTimestamp = timestamp + static_cast<int64_t>(event.ts.nanosec / 1000);
		dv::runtime_assert(eventTimestamp == toDvTime(event.ts), "Timestamp conversion failed!");
		eventPacket->elements.emplace_back(eventTimestamp, event.x, event.y, event.polarity);
	}
	dv::EventStore store(std::const_pointer_cast<const dv::EventPacket>(eventPacket));
	return store;
}

/// @brief Convert an image message from ROS2 into a dv::Frame. Allocates the memory for the image and performs
///        deep data copy.
/// @param imageMsg Message to be converted.
/// @return A copy of the image in a dv::Frame.
/// @throws runtime_error Exception is thrown if encoding of the source image is not supported.
[[nodiscard]] inline dv::Frame toDvFrame(const sensor_msgs::msg::Image &imageMsg) 
{
	const std::string encoding(imageMsg.encoding);
	cv::Mat image(static_cast<int32_t>(imageMsg.height), static_cast<int32_t>(imageMsg.width),
		_detail::imageTypeFromEncoding(encoding), const_cast<uchar *>(&imageMsg.data[0]), imageMsg.step);
	return {toDvTime(imageMsg.header.stamp), image.clone()};
}


/// @brief A small convenience class that allows image memory access through cv::Mat mapping. The underlying data is valid
///        as long as the instance of this class exists, since it does not perform deep memory copy, just shares the
///        ownership by keeping a copy of a smart pointer to the actual memory, so the memory is not deallocated.
class FrameMap {
public:
	
	/// @brief A smart pointer to the original memory owner.
	sensor_msgs::msg::Image::ConstPtr message;

	/// @brief This frame is a mapping to the contents of image message. This value should not be used outside
	///        of this map class and it is only valid as long as the parent class instance exists.
	dv::Frame frame;

	/// @brief Construct the mapping, it will construct the mapped frame in this class instance.
	/// @param msg Image message for mapping.
	explicit FrameMap(const sensor_msgs::msg::Image::ConstPtr &msg) : message(msg) {
		const std::string encoding(msg->encoding);
		cv::Mat image(static_cast<int32_t>(msg->height), static_cast<int32_t>(msg->width),
			_detail::imageTypeFromEncoding(encoding), const_cast<uchar *>(&msg->data[0]), msg->step);
		frame = dv::Frame(toDvTime(msg->header.stamp), image);
	}
};

} // namespace dv_ros2_msgs
