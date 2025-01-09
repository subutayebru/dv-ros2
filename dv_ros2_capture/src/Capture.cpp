#include "dv_ros2_capture/Capture.hpp"

namespace dv_ros2_capture
{
    Capture::Capture(const std::string &t_node_name) 
    : Node(t_node_name), m_node{this}
    {
        m_spin_thread = true;
        RCLCPP_INFO(m_node->get_logger(), "Constructor is initialized");
        parameterInitilization();

        if (!readParameters())
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameters");
            rclcpp::shutdown();
            std::exit(EXIT_FAILURE);
        }

        parameterPrinter();

        if (m_params.aedat4FilePath.empty())
        {
            m_reader = Reader(m_params.cameraName);
        }
        else 
        {
            m_reader = Reader(m_params.aedat4FilePath, m_params.cameraName);
        }
        startup_time = m_node->now();

        if (m_params.frames && !m_reader.isFrameStreamAvailable())
        {
            m_params.frames = false;
            RCLCPP_WARN(m_node->get_logger(), "Frame stream is not available.");
        }
        if (m_params.events && !m_reader.isEventStreamAvailable())
        {
            m_params.events = false;
            RCLCPP_WARN(m_node->get_logger(), "Event stream is not available.");
        }
        if (m_params.imu && !m_reader.isImuStreamAvailable())
        {
            m_params.imu = false;
            RCLCPP_WARN(m_node->get_logger(), "IMU stream is not available.");
        }
        if (m_params.triggers && !m_reader.isTriggerStreamAvailable())
        {
            m_params.triggers = false;
            RCLCPP_WARN(m_node->get_logger(), "Trigger stream is not available.");
        }

        if (m_params.frames)
        {
            m_frame_publisher = m_node->create_publisher<sensor_msgs::msg::Image>("frame", 10);
        }
        //if (m_params.events)
        //{
        //    m_events_publisher = m_node->create_publisher<dv_ros2_msgs::msg::EventArray>("events", 10);
        //}
        if (m_params.events)
        {
            m_events_publisher = m_node->create_publisher<dv_ros2_msgs::msg::EventPacket>("events", 10);
        }
        if (m_params.triggers)
        {
            m_trigger_publisher = m_node->create_publisher<dv_ros2_msgs::msg::Trigger>("trigger", 10);
        }
        if (m_params.imu)
        {
            m_imu_publisher = m_node->create_publisher<sensor_msgs::msg::Imu>("imu", 10);
        }
        m_camera_info_publisher = m_node->create_publisher<sensor_msgs::msg::CameraInfo>("camera_info", 10);
        m_set_imu_biases_service = m_node->create_service<dv_ros2_msgs::srv::SetImuBiases>("set_imu_biases", std::bind(&Capture::setImuBiases, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        m_set_imu_info_service= m_node->create_service<dv_ros2_msgs::srv::SetImuInfo>("set_imu_info", std::bind(&Capture::setImuInfo, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        m_set_camera_info_service = m_node->create_service<sensor_msgs::srv::SetCameraInfo>("set_camera_info", std::bind(&Capture::setCameraInfo, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        fs::path calibrationPath = getActiveCalibrationPath();
        if (!m_params.cameraCalibrationFilePath.empty())
        {
            RCLCPP_INFO_STREAM(m_node->get_logger(),"Loading user supplied calibration at path [" << m_params.cameraCalibrationFilePath << "]");
            if (!fs::exists(m_params.cameraCalibrationFilePath))
            {
                throw dv::exceptions::InvalidArgument<std::string>("User supplied calibration file does not exist!", m_params.cameraCalibrationFilePath);
            }
            RCLCPP_INFO_STREAM(m_node->get_logger(), "Loading calibration data from [" << m_params.cameraCalibrationFilePath << "]");
            fs::copy_file(m_params.cameraCalibrationFilePath, calibrationPath, fs::copy_options::overwrite_existing);
        }

        if (fs::exists(calibrationPath))
        {
            RCLCPP_INFO_STREAM(m_node->get_logger(), "Loading calibration data from [" << calibrationPath << "]");
            m_calibration = dv::camera::CalibrationSet::LoadFromFile(calibrationPath);
            const std::string cameraName = m_reader.getCameraName();
            auto cameraCalibration = m_calibration.getCameraCalibrationByName(cameraName);
            if (const auto &imuCalib = m_calibration.getImuCalibrationByName(cameraName); imuCalib.has_value())
            {
                m_transform_publisher = m_node->create_publisher<tf2_msgs::msg::TFMessage>("/tf", 100);
                m_imu_time_offset == imuCalib->timeOffsetMicros;

                geometry_msgs::msg::TransformStamped msg;
                msg.header.frame_id = m_params.imuFrameName;
                msg.child_frame_id = m_params.cameraFrameName;

                m_imu_to_cam_transform = dv::kinematics::Transformationf(0, Eigen::Matrix<float, 4, 4, Eigen::RowMajor>(imuCalib->transformationToC0.data()));

                m_acc_biases.x() = imuCalib->accOffsetAvg.x;
                m_acc_biases.y() = imuCalib->accOffsetAvg.y;
                m_acc_biases.z() = imuCalib->accOffsetAvg.z;

                m_gyro_biases.x() = imuCalib->omegaOffsetAvg.x;
                m_gyro_biases.y() = imuCalib->omegaOffsetAvg.y;
                m_gyro_biases.z() = imuCalib->omegaOffsetAvg.z;

                const auto translation = m_imu_to_cam_transform.getTranslation<Eigen::Vector3d>();
                msg.transform.translation.x = translation.x();
                msg.transform.translation.y = translation.y();
                msg.transform.translation.z = translation.z();

                const auto rotation = m_imu_to_cam_transform.getQuaternion();
                msg.transform.rotation.x = rotation.x();
                msg.transform.rotation.y = rotation.y();
                msg.transform.rotation.z = rotation.z();
                msg.transform.rotation.w = rotation.w();

                m_imu_to_cam_transforms = tf2_msgs::msg::TFMessage();
                m_imu_to_cam_transforms->transforms.push_back(msg);
            }
            if (cameraCalibration.has_value())
            {
                populateInfoMsg(cameraCalibration->getCameraGeometry());
            }
            else
            {
                RCLCPP_ERROR_STREAM(m_node->get_logger(), "Calibration in [" << calibrationPath << "] does not contain calibration for camera [" << cameraName << "]");
                std::vector<std::string> names;
                for (const auto &calib : m_calibration.getCameraCalibrations())
                {
                    names.push_back(calib.second.name);
                }
                const std::string nameString = fmt::format("{}", fmt::join(names, "; "));
                RCLCPP_ERROR_STREAM(m_node->get_logger(), "The file only contains calibrations for these cameras: [" << nameString << "]");
                throw std::runtime_error("Calibration is not available!");
            }
        }
        else
        {
            RCLCPP_WARN_STREAM(m_node->get_logger(), "[" << m_reader.getCameraName() << "] No calibration was found, assuming ideal pinhole (no distortion).");
            std::optional<cv::Size> resolution;
            if (m_reader.isFrameStreamAvailable())
            {
                resolution = m_reader.getFrameResolution();
            }
            else if (m_reader.isEventStreamAvailable())
            {
                resolution = m_reader.getEventResolution();
            }
            if (resolution.has_value())
            {
                const auto width = static_cast<float>(resolution->width);
                populateInfoMsg(dv::camera::CameraGeometry(width, width, width * 0.5f, static_cast<float>(resolution->height) * 0.5f, *resolution));
                generateActiveCalibrationFile();
            }
            else
            {
                throw std::runtime_error("Sensor resolution not available.");
            }
        }

        auto& camera_ptr = m_reader.getCameraCapturePtr();
        if (camera_ptr != nullptr) {
            if (camera_ptr->isFrameStreamAvailable()) 
            {
                // DAVIS camera
                if (camera_ptr->isTriggerStreamAvailable()) {
                    // External trigger detection support for DAVIS346 - MODIFY HERE FOR DIFFERENT DETECTION SETTINGS!
                    camera_ptr->deviceConfigSet(DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_RISING_EDGES, true);
                    camera_ptr->deviceConfigSet(DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_FALLING_EDGES, false);
                    camera_ptr->deviceConfigSet(DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_DETECT_PULSES, false);
                    camera_ptr->deviceConfigSet(DAVIS_CONFIG_EXTINPUT, DAVIS_CONFIG_EXTINPUT_RUN_DETECTOR, m_params.triggers);
                }
            }
            else {
                // DVXplorer type camera
                if (camera_ptr->isTriggerStreamAvailable()) {
                    // External trigger detection support for DVXplorer - MODIFY HERE FOR DIFFERENT DETECTION SETTINGS!
                    camera_ptr->deviceConfigSet(DVX_EXTINPUT, DVX_EXTINPUT_DETECT_RISING_EDGES, true);
                    camera_ptr->deviceConfigSet(DVX_EXTINPUT, DVX_EXTINPUT_DETECT_FALLING_EDGES, false);
                    camera_ptr->deviceConfigSet(DVX_EXTINPUT, DVX_EXTINPUT_DETECT_PULSES, false);
                    camera_ptr->deviceConfigSet(DVX_EXTINPUT, DVX_EXTINPUT_RUN_DETECTOR, m_params.triggers);
                }
            }
            updateConfiguration();
        }

        RCLCPP_INFO(m_node->get_logger(), "Successfully launched.");
    }

    Capture::~Capture()
    {
        RCLCPP_INFO(m_node->get_logger(), "Destructor is initialized");
        stop();
        rclcpp::shutdown();
    }

    void Capture::stop()
    {
        RCLCPP_INFO(m_node->get_logger(), "Stopping the capture node...");
        m_spin_thread = false;
        m_clock.join();
        if (m_params.frames)
        {
            m_frame_thread.join();
        }
        if (m_params.events)
        {
            m_events_thread.join();
        }
        if (m_params.triggers)
        {
            m_trigger_thread.join();
        }
        if (m_params.imu)
        {
            m_imu_thread.join();
        }
        if (m_sync_thread.joinable())
        {
            m_sync_thread.join();
        }
        if (m_camera_info_thread != nullptr)
        {
            m_camera_info_thread->join();
        }
        if (m_discovery_thread != nullptr)
        {
            m_discovery_thread->join();
        }
    }

    inline void Capture::parameterInitilization() const
    {
        rcl_interfaces::msg::ParameterDescriptor descriptor;
        rcl_interfaces::msg::IntegerRange int_range;

        int_range.set__from_value(1).set__to_value(1000000).set__step(1);
        descriptor.integer_range = {int_range};
        m_node->declare_parameter("time_increment", m_params.timeIncrement, descriptor);
        m_node->declare_parameter("frames", m_params.frames);
        m_node->declare_parameter("events", m_params.events);
        m_node->declare_parameter("imu", m_params.imu);
        m_node->declare_parameter("triggers", m_params.triggers);
        m_node->declare_parameter("camera_name", m_params.cameraName);
        m_node->declare_parameter("aedat4_file_path", m_params.aedat4FilePath);
        m_node->declare_parameter("camera_calibration_file_path", m_params.cameraCalibrationFilePath);
        m_node->declare_parameter("camera_frame_name", m_params.cameraFrameName);
        m_node->declare_parameter("imu_frame_name", m_params.imuFrameName);
        m_node->declare_parameter("transform_imu_to_camera_frame", m_params.transformImuToCameraFrame);
        m_node->declare_parameter("unbiased_imu_data", m_params.unbiasedImuData);
        m_node->declare_parameter("noise_filtering", m_params.noiseFiltering);
        int_range.set__from_value(1).set__to_value(1000000).set__step(1);
        descriptor.integer_range = {int_range};
        m_node->declare_parameter("noise_ba_time", m_params.noiseBATime, descriptor);
        m_node->declare_parameter("sync_device_list", m_params.syncDeviceList);
        m_node->declare_parameter("wait_for_sync", m_params.waitForSync);
        m_node->declare_parameter("global_hold", m_params.globalHold);
        int_range.set__from_value(0).set__to_value(5).set__step(1);
        descriptor.integer_range = {int_range};
        m_node->declare_parameter("bias_sensitivity", m_params.biasSensitivity, descriptor);
    }

    inline void Capture::parameterPrinter() const
    {
        RCLCPP_INFO(m_node->get_logger(), "---- Parameters ----");
        RCLCPP_INFO(m_node->get_logger(), "time_increment: %d", static_cast<int>(m_params.timeIncrement));
        RCLCPP_INFO(m_node->get_logger(), "frames: %s", m_params.frames ? "true" : "false");
        RCLCPP_INFO(m_node->get_logger(), "events: %s", m_params.events ? "true" : "false");
        RCLCPP_INFO(m_node->get_logger(), "imu: %s", m_params.imu ? "true" : "false");
        RCLCPP_INFO(m_node->get_logger(), "triggers: %s", m_params.triggers ? "true" : "false");
        RCLCPP_INFO(m_node->get_logger(), "camera_name: %s", m_params.cameraName.c_str());
        RCLCPP_INFO(m_node->get_logger(), "aedat4_file_path: %s", m_params.aedat4FilePath.c_str());
        RCLCPP_INFO(m_node->get_logger(), "camera_calibration_file_path: %s", m_params.cameraCalibrationFilePath.c_str());
        RCLCPP_INFO(m_node->get_logger(), "camera_frame_name: %s", m_params.cameraFrameName.c_str());
        RCLCPP_INFO(m_node->get_logger(), "imu_frame_name: %s", m_params.imuFrameName.c_str());
        RCLCPP_INFO(m_node->get_logger(), "transform_imu_to_camera_frame: %s", m_params.transformImuToCameraFrame ? "true" : "false");
        RCLCPP_INFO(m_node->get_logger(), "unbiased_imu_data: %s", m_params.unbiasedImuData ? "true" : "false");
        RCLCPP_INFO(m_node->get_logger(), "noise_filtering: %s", m_params.noiseFiltering ? "true" : "false");
        RCLCPP_INFO(m_node->get_logger(), "noise_ba_time: %d", static_cast<int>(m_params.noiseBATime));
        RCLCPP_INFO(m_node->get_logger(), "sync_device_list: ");
        for (const auto &device : m_params.syncDeviceList)
        {
            RCLCPP_INFO(m_node->get_logger(), "  %s", device.c_str());
        }
        RCLCPP_INFO(m_node->get_logger(), "wait_for_sync: %s", m_params.waitForSync ? "true" : "false");
        RCLCPP_INFO(m_node->get_logger(), "global_hold: %s", m_params.globalHold ? "true" : "false");
        RCLCPP_INFO(m_node->get_logger(), "bias_sensitivity: %d", m_params.biasSensitivity);
    }

    inline bool Capture::readParameters()
    {
        if (!m_node->get_parameter("time_increment", m_params.timeIncrement))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter time_increment");
            return false;
        }
        if (!m_node->get_parameter("frames", m_params.frames))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter frames");
            return false;
        }
        if (!m_node->get_parameter("events", m_params.events))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter events");
            return false;
        }
        if (!m_node->get_parameter("imu", m_params.imu))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter imu");
            return false;
        }
        if (!m_node->get_parameter("triggers", m_params.triggers))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter triggers");
            return false;
        }
        if (!m_node->get_parameter("camera_name", m_params.cameraName))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter camera_name");
            return false;
        }
        if (!m_node->get_parameter("aedat4_file_path", m_params.aedat4FilePath))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter aedat4_file_path");
            return false;
        }
        if (!m_node->get_parameter("camera_calibration_file_path", m_params.cameraCalibrationFilePath))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter camera_calibration_file_path");
            return false;
        }
        if (!m_node->get_parameter("camera_frame_name", m_params.cameraFrameName))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter camera_frame_name");
            return false;
        }
        if (!m_node->get_parameter("imu_frame_name", m_params.imuFrameName))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter imu_frame_name");
            return false;
        }
        if (!m_node->get_parameter("transform_imu_to_camera_frame", m_params.transformImuToCameraFrame))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter transform_imu_to_camera_frame");
            return false;
        }
        if (!m_node->get_parameter("unbiased_imu_data", m_params.unbiasedImuData))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter unbiased_imu_data");
            return false;
        }
        if (!m_node->get_parameter("noise_filtering", m_params.noiseFiltering))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter noise_filtering");
            return false;
        }
        if (!m_node->get_parameter("noise_ba_time", m_params.noiseBATime))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter noise_ba_time");
            return false;
        }
        if (!m_node->get_parameter("sync_device_list", m_params.syncDeviceList))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter sync_device_list");
            return false;
        }
        if (!m_node->get_parameter("wait_for_sync", m_params.waitForSync))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter wait_for_sync");
            return false;
        }
        if (!m_node->get_parameter("global_hold", m_params.globalHold))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter global_hold");
            return false;
        }
        if (!m_node->get_parameter("bias_sensitivity", m_params.biasSensitivity))
        {
            RCLCPP_ERROR(m_node->get_logger(), "Failed to read parameter biasSensitivity");
            return false;
        }
        return true;
    }

    void Capture::updateConfiguration()
    {
        RCLCPP_INFO(m_node->get_logger(), "Updating configuration...");
        auto& camera_ptr = m_reader.getCameraCapturePtr();
        if (camera_ptr != nullptr)
        {
            if (camera_ptr->isFrameStreamAvailable()) 
            {
                // DAVIS camera
                camera_ptr->setDVSGlobalHold(m_params.globalHold);
                camera_ptr->setDVSBiasSensitivity(static_cast<dv::io::CameraCapture::BiasSensitivity>(m_params.biasSensitivity));
                updateNoiseFilter(m_params.noiseFiltering, static_cast<int64_t>(m_params.noiseBATime));
            }
            else {
                // DVXplorer type camera
                camera_ptr->setDVSGlobalHold(m_params.globalHold);
                camera_ptr->setDVSBiasSensitivity(static_cast<dv::io::CameraCapture::BiasSensitivity>(m_params.biasSensitivity));
                updateNoiseFilter(m_params.noiseFiltering, static_cast<int64_t>(m_params.noiseBATime));
            }

            // Support variable data interval sizes.
            camera_ptr->deviceConfigSet(CAER_HOST_CONFIG_PACKETS, CAER_HOST_CONFIG_PACKETS_MAX_CONTAINER_INTERVAL, m_params.timeIncrement);
        }
    }

    rcl_interfaces::msg::SetParametersResult Capture::paramsCallback(const std::vector<rclcpp::Parameter> &parameters)
    {
        rcl_interfaces::msg::SetParametersResult result;
        result.successful = true;
        result.reason = "success";

        for (const auto &param : parameters)
        {
            if (param.get_name() == "time_increment")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_INTEGER)
                {
                    m_params.timeIncrement = param.as_int();
                }
                else
                {
                    result.successful = false;
                    result.reason = "time_increment must be an integer";
                }
            }
            else if (param.get_name() == "frames")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL)
                {
                    m_params.frames = param.as_bool();
                }
                else
                {
                    result.successful = false;
                    result.reason = "frames must be a boolean";
                }
            }
            else if (param.get_name() == "events")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL)
                {
                    m_params.events = param.as_bool();
                }
                else
                {
                    result.successful = false;
                    result.reason = "events must be a boolean";
                }
            }
            else if (param.get_name() == "imu")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL)
                {
                    m_params.imu = param.as_bool();
                }
                else
                {
                    result.successful = false;
                    result.reason = "imu must be a boolean";
                }
            }
            else if (param.get_name() == "triggers")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL)
                {
                    m_params.triggers = param.as_bool();
                }
                else
                {
                    result.successful = false;
                    result.reason = "triggers must be a boolean";
                }
            }
            else if (param.get_name() == "camera_name")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_STRING)
                {
                    m_params.cameraName = param.as_string();
                }
                else
                {
                    result.successful = false;
                    result.reason = "camera_name must be a string";
                }
            }
            else if (param.get_name() == "aedat4_file_path")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_STRING)
                {
                    m_params.aedat4FilePath = param.as_string();
                }
                else
                {
                    result.successful = false;
                    result.reason = "aedat4_file_path must be a string";
                }
            }
            else if (param.get_name() == "camera_calibration_file_path")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_STRING)
                {
                    m_params.cameraCalibrationFilePath = param.as_string();
                }
                else
                {
                    result.successful = false;
                    result.reason = "camera_calibration_file_path must be a string";
                }
            }
            else if (param.get_name() == "camera_frame_name")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_STRING)
                {
                    m_params.cameraFrameName = param.as_string();
                }
                else
                {
                    result.successful = false;
                    result.reason = "camera_frame_name must be a string";
                }
            }
            else if (param.get_name() == "imu_frame_name")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_STRING)
                {
                    m_params.imuFrameName = param.as_string();
                }
                else
                {
                    result.successful = false;
                    result.reason = "imu_frame_name must be a string";
                }
            }
            else if (param.get_name() == "transform_imu_to_camera_frame")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL)
                {
                    m_params.transformImuToCameraFrame = param.as_bool();
                }
                else
                {
                    result.successful = false;
                    result.reason = "transform_imu_to_camera_frame must be a boolean";
                }
            }
            else if (param.get_name() == "unbiased_imu_data")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL)
                {
                    m_params.unbiasedImuData = param.as_bool();
                }
                else
                {
                    result.successful = false;
                    result.reason = "unbiased_imu_data must be a boolean";
                }
            }
            else if (param.get_name() == "noise_filtering")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL)
                {
                    m_params.noiseFiltering = param.as_bool();
                }
                else
                {
                    result.successful = false;
                    result.reason = "noise_filtering must be a boolean";
                }
            }
            else if (param.get_name() == "noise_ba_time")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_INTEGER)
                {
                    m_params.noiseBATime = param.as_int();
                }
                else
                {
                    result.successful = false;
                    result.reason = "noise_ba_time must be an integer";
                }
            }
            else if (param.get_name() == "sync_device_list")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_STRING_ARRAY)
                {
                    m_params.syncDeviceList = param.as_string_array();
                }
                else
                {
                    result.successful = false;
                    result.reason = "sync_device_list must be a string array";
                }
            }
            else if (param.get_name() == "wait_for_sync")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL)
                {
                    m_params.waitForSync = param.as_bool();
                }
                else
                {
                    result.successful = false;
                    result.reason = "wait_for_sync must be a boolean";
                }
            }
            else if (param.get_name() == "global_hold")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_BOOL)
                {
                    m_params.globalHold = param.as_bool();
                }
                else
                {
                    result.successful = false;
                    result.reason = "global_hold must be a boolean";
                }
            }
            else if (param.get_name() == "bias_sensitivity")
            {
                if (param.get_type() == rclcpp::ParameterType::PARAMETER_INTEGER)
                {
                    m_params.biasSensitivity = param.as_int();
                }
                else
                {
                    result.successful = false;
                    result.reason = "bias_sensitivity must be an integer";
                }
            }
            else
            {
                result.successful = false;
                result.reason = "unknown parameter";
            }
        }
        updateConfiguration();
        return result;
    }

    void Capture::populateInfoMsg(const dv::camera::CameraGeometry &cameraGeometry)
    {
        m_camera_info_msg.width = cameraGeometry.getResolution().width;
        m_camera_info_msg.height = cameraGeometry.getResolution().height;

        const auto distortion = cameraGeometry.getDistortion();

        switch (cameraGeometry.getDistortionModel()) 
        {
            case dv::camera::DistortionModel::Equidistant: {
                m_camera_info_msg.distortion_model = sensor_msgs::distortion_models::EQUIDISTANT;
                m_camera_info_msg.d.assign(distortion.begin(), distortion.end());
                break;
            }

            case dv::camera::DistortionModel::RadTan: {
                m_camera_info_msg.distortion_model = sensor_msgs::distortion_models::PLUMB_BOB;
                m_camera_info_msg.d.assign(distortion.begin(), distortion.end());
                if (m_camera_info_msg.d.size() < 5) {
                    m_camera_info_msg.d.resize(5, 0.0);
                }
                break;
            }

            case dv::camera::DistortionModel::None: {
                m_camera_info_msg.distortion_model = sensor_msgs::distortion_models::PLUMB_BOB;
                m_camera_info_msg.d                = {0.0, 0.0, 0.0, 0.0, 0.0};
                break;
            }

            default:
                throw dv::exceptions::InvalidArgument<dv::camera::DistortionModel>(
                    "Unsupported camera distortion model.", cameraGeometry.getDistortionModel());
	}

        auto cx = cameraGeometry.getCentralPoint().x;
        auto cy = cameraGeometry.getCentralPoint().y;
        auto fx = cameraGeometry.getFocalLength().x;
        auto fy = cameraGeometry.getFocalLength().y;

        m_camera_info_msg.k = {fx, 0, cx, 0, fy, cy, 0, 0, 1};
        m_camera_info_msg.r = {1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0};
        m_camera_info_msg.p = {fx, 0, cx, 0, 0, fy, cy, 0, 0, 0, 1.0, 0};
    }

    sensor_msgs::msg::Imu Capture::transformImuFrame(sensor_msgs::msg::Imu &&imu)
    {
        if (m_params.unbiasedImuData)
        {
            imu.linear_acceleration.x -= m_acc_biases.x();
            imu.linear_acceleration.y -= m_acc_biases.y();
            imu.linear_acceleration.z -= m_acc_biases.z();

            imu.angular_velocity.x -= m_gyro_biases.x();
            imu.angular_velocity.y -= m_gyro_biases.y();
            imu.angular_velocity.z -= m_gyro_biases.z();
        }
        if (m_params.transformImuToCameraFrame)
        {
            const Eigen::Vector3<double> resW 
                = m_imu_to_cam_transform.rotatePoint<Eigen::Vector3<double>>(imu.angular_velocity);
            imu.angular_velocity.x = resW.x();
            imu.angular_velocity.y = resW.y();
            imu.angular_velocity.z = resW.z();

            const Eigen::Vector3<double> resV
                = m_imu_to_cam_transform.rotatePoint<Eigen::Vector3<double>>(imu.linear_acceleration);
            imu.linear_acceleration.x = resV.x();
            imu.linear_acceleration.y = resV.y();
            imu.linear_acceleration.z = resV.z();
        }
        return imu;
    }
    
    void Capture::updateCalibrationSet()
    {
        RCLCPP_INFO(m_node->get_logger(), "Updating calibration set...");
        const std::string cameraName = m_reader.getCameraName();
        dv::camera::calibrations::CameraCalibration calib;
        bool calibrationExists = false;
        if (auto camCalibration = m_calibration.getCameraCalibrationByName(cameraName); camCalibration.has_value()) 
        {
            calib             = *camCalibration;
            calibrationExists = true;
        }
        else 
        {
            calib.name = cameraName;
        }
        calib.resolution = cv::Size(static_cast<int>(m_camera_info_msg.width), static_cast<int>(m_camera_info_msg.height));
        calib.distortion.clear();
        calib.distortion.assign(m_camera_info_msg.d.begin(), m_camera_info_msg.d.end());
        if (static_cast<std::string>(m_camera_info_msg.distortion_model) == sensor_msgs::distortion_models::PLUMB_BOB) 
        {
            calib.distortionModel = dv::camera::DistortionModel::RadTan;
        }
        else if (static_cast<std::string>(m_camera_info_msg.distortion_model) == sensor_msgs::distortion_models::EQUIDISTANT) 
        {
            calib.distortionModel = dv::camera::DistortionModel::Equidistant;
        }
        else 
        {
            throw dv::exceptions::InvalidArgument<sensor_msgs::msg::CameraInfo::_distortion_model_type>(
                "Unknown camera model.", m_camera_info_msg.distortion_model);
        }
        calib.focalLength = cv::Point2f(static_cast<float>(m_camera_info_msg.k[0]), static_cast<float>(m_camera_info_msg.k[4]));
        calib.principalPoint
            = cv::Point2f(static_cast<float>(m_camera_info_msg.k[2]), static_cast<float>(m_camera_info_msg.k[5]));

        calib.transformationToC0 = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};

        if (calibrationExists) 
        {
            m_calibration.updateCameraCalibration(calib);
        }
        else 
        {
            m_calibration.addCameraCalibration(calib);
        }

        dv::camera::calibrations::IMUCalibration imuCalibration;
        bool imuCalibrationExists = false;
        if (auto imuCalib = m_calibration.getImuCalibrationByName(cameraName); imuCalib.has_value()) 
        {
            imuCalibration       = *imuCalib;
            imuCalibrationExists = true;
        }
        else 
        {
            imuCalibration.name = cameraName;
        }
        bool imuHasValues = false;
        if ((m_imu_to_cam_transforms.has_value() && !m_imu_to_cam_transforms->transforms.empty())) 
        {
            const Eigen::Matrix4f mat         = m_imu_to_cam_transform.getTransform().transpose();
            imuCalibration.transformationToC0 = std::vector<float>(mat.data(), mat.data() + mat.rows() * mat.cols());
            imuHasValues                      = true;
        }

        if (!m_acc_biases.isZero()) 
        {
            imuCalibration.accOffsetAvg.x = m_acc_biases.x();
            imuCalibration.accOffsetAvg.y = m_acc_biases.y();
            imuCalibration.accOffsetAvg.z = m_acc_biases.z();
            imuHasValues                  = true;
        }

        if (!m_gyro_biases.isZero()) 
        {
            imuCalibration.omegaOffsetAvg.x = m_gyro_biases.x();
            imuCalibration.omegaOffsetAvg.y = m_gyro_biases.y();
            imuCalibration.omegaOffsetAvg.z = m_gyro_biases.z();
            imuHasValues                    = true;
        }

        if (m_imu_time_offset > 0) 
        {
            imuCalibration.timeOffsetMicros = m_imu_time_offset;
            imuHasValues                    = true;
        }

        if (imuCalibrationExists) 
        {
            m_calibration.updateImuCalibration(imuCalibration);
        }
        else if (imuHasValues) 
        {
            m_calibration.addImuCalibration(imuCalibration);
        }
    }

    void Capture::startCapture()
    {
        RCLCPP_INFO(m_node->get_logger(), "Spinning capture node...");
        auto times = m_reader.getTimeRange();

        const auto &live_capture = m_reader.getCameraCapturePtr();

        if (live_capture)
        {
            m_synchronized = false;
            live_capture->setDVXplorerEFPS(dv::io::CameraCapture::DVXeFPS::EFPS_CONSTANT_500);
            m_sync_thread = std::thread(&Capture::synchronizationThread, this);
        }
        else 
        {
            m_synchronized = true;
        }

        if (times.has_value())
        {
            m_clock = std::thread(&Capture::clock, this, times->first, times->second, m_params.timeIncrement);
        }
        else
        {
            m_clock = std::thread(&Capture::clock, this, -1, -1, m_params.timeIncrement);
        }
        if (m_params.frames)
        {
            m_frame_thread = std::thread(&Capture::framePublisher, this);
        }
        if (m_params.events)
        {
            m_events_thread = std::thread(&Capture::eventsPublisher, this);
        }
        if (m_params.triggers)
        {
            m_trigger_thread = std::thread(&Capture::triggerPublisher, this);
        }
        if (m_params.imu)
        {
            m_imu_thread = std::thread(&Capture::imuPublisher, this);
        }

        if (m_params.events || m_params.frames) 
        {
            RCLCPP_INFO(m_node->get_logger(), "Spinning camera info thread.");
            m_camera_info_thread = std::make_unique<std::thread>([this] 
            {
                rclcpp::Rate infoRate(25.0);
                while (m_spin_thread.load(std::memory_order_relaxed))
                {
                    const rclcpp::Time currentTime = dv_ros2_msgs::toRosTime(m_current_seek);
                    if (m_camera_info_publisher->get_subscription_count() > 0)
                    {
                        m_camera_info_msg.header.stamp = currentTime;
                        m_camera_info_publisher->publish(m_camera_info_msg);
                    }
                    if (m_imu_to_cam_transforms.has_value() && !m_imu_to_cam_transforms->transforms.empty())
                    {
                        m_imu_to_cam_transforms->transforms.back().header.stamp = currentTime;
                        m_transform_publisher->publish(*m_imu_to_cam_transforms);
                    }
                    infoRate.sleep();
                }
            });
        }
    }

    bool Capture::isRunning() const
    {
        return m_spin_thread.load(std::memory_order_relaxed);
    }

    void Capture::clock(int64_t start, int64_t end, int64_t timeIncrement)
    {
        RCLCPP_INFO(m_node->get_logger(), "Spinning clock.");

        double frequency = 1.0 / (static_cast<double>(timeIncrement) * 1e-6);
        
        rclcpp::Rate sleepRate(frequency);
        if (start == -1)
        {
            start = std::numeric_limits<int64_t>::max() - 1;
            end = std::numeric_limits<int64_t>::max();
            timeIncrement = 0;
            RCLCPP_INFO_STREAM(m_node->get_logger(), "Reading from camera [" << m_reader.getCameraName() << "]");
        }

        while (m_spin_thread)
        {
            if (m_synchronized.load(std::memory_order_relaxed))
            {
                if (m_params.frames)
                {
                    m_frame_queue.push(start);
                }
                if (m_params.events)
                {
                    m_events_queue.push(start);
                }
                if (m_params.triggers)
                {
                    m_trigger_queue.push(start);
                }
                if (m_params.imu)
                {
                    m_imu_queue.push(start);
                }
                start += timeIncrement;
            }

            sleepRate.sleep();

            if (start >= end || !m_reader.isConnected())
            {
                m_spin_thread = false;
            }
        }

    }

    void Capture::runDiscovery(const std::string &syncServiceName)
    {
        const auto &liveCapture = m_reader.getCameraCapturePtr();

        if (liveCapture == nullptr)
        {
            return;
        }

        m_discovery_publisher = m_node->create_publisher<dv_ros2_msgs::msg::CameraDiscovery>("/dvs/discovery", 10);
        m_discovery_thread = std::make_unique<std::thread>([this, &liveCapture, &syncServiceName] 
        {
            dv_ros2_msgs::msg::CameraDiscovery message;
            message.is_master = liveCapture->isMasterCamera();
            message.name = liveCapture->getCameraName();
            message.startup_time = startup_time;
            message.publishing_events = m_params.events;
            message.publishing_frames = m_params.frames;
            message.publishing_imu = m_params.imu;
            message.publishing_triggers = m_params.triggers;
            message.sync_service_topic = syncServiceName;
            // 5 Hz is enough
            rclcpp::Rate rate(5.0);
            while (m_spin_thread)
            {
                if (m_discovery_publisher->get_subscription_count() > 0)
                {
                    // message.header.seq++; seq removed in ROS2
                    message.header.stamp = m_node->now();
                    m_discovery_publisher->publish(message);
                }
                rate.sleep();
            }
        });
    }

    std::map<std::string, std::string> Capture::discoverSyncDevices() const 
    {
        if (m_params.syncDeviceList.empty() || m_params.syncDeviceList[0] == "") 
        {
            return {};
        }

        RCLCPP_INFO_STREAM(m_node->get_logger(), "Waiting for devices [" << fmt::format("{}", fmt::join(m_params.syncDeviceList, ", ")) << "] to be online...");

        // List info about each sync device
        struct DiscoveryContext 
        {
            std::map<std::string, std::string> serviceNames;
            std::atomic<bool> complete;
            std::vector<std::string> deviceList;

            void handleMessage(const dv_ros2_msgs::msg::CameraDiscovery::SharedPtr message) 
            {
                const std::string cameraName(message->name.c_str());
                if (serviceNames.contains(cameraName)) 
                {
                    return;
                }

                if (std::find(deviceList.begin(), deviceList.end(), cameraName) != deviceList.end()) 
                {
                    serviceNames.insert(std::make_pair(cameraName, message->sync_service_topic.c_str()));
                    if (serviceNames.size() == deviceList.size()) 
                    {
                        complete = true;
                    }
                }
            }
        };

        DiscoveryContext context;
        context.deviceList = m_params.syncDeviceList;
        context.complete   = false;

        auto subscriber = m_node->create_subscription<dv_ros2_msgs::msg::CameraDiscovery>("/dvs/discovery", 10, std::bind(&DiscoveryContext::handleMessage, &context, std::placeholders::_1));

        while (m_spin_thread.load(std::memory_order_relaxed) && !context.complete.load(std::memory_order_relaxed)) 
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        RCLCPP_INFO(m_node->get_logger(), "All sync devices are online.");

        return context.serviceNames;
    }

    bool Capture::setCameraInfo(const std::shared_ptr<rmw_request_id_t> request_header,
                               const std::shared_ptr<sensor_msgs::srv::SetCameraInfo::Request> req,
                               std::shared_ptr<sensor_msgs::srv::SetCameraInfo::Response> rsp)
    {
        // Set camera info is usually called from a camera calibration pipeline.
        (void)request_header;
        m_camera_info_msg = req->camera_info;

        try
        {
            auto calibPath = saveCalibration();
            rsp->success = true;
            rsp->status_message = fmt::format("Calibration stored successfully in [{}].", calibPath);
        }
        catch (const std::exception &e)
        {
            rsp->success = false;
            rsp->status_message = fmt::format("Error storing camera calibration: [{}].", e.what());
        }

        return true;
    }

    bool Capture::setImuBiases(const std::shared_ptr<rmw_request_id_t> request_header,
                               const std::shared_ptr<dv_ros2_msgs::srv::SetImuBiases::Request> req,
                               std::shared_ptr<dv_ros2_msgs::srv::SetImuBiases::Response> rsp)
    {
        // Set Imu biases is called by a node that computes the biases. Hence,
        // only the Imu biases are changed.
        (void)request_header;

        if (m_params.unbiasedImuData)
        {
            RCLCPP_ERROR(m_node->get_logger(), "Trying to set IMU biases on a camera capture node which publishes IMU data with biases subtracted.");
            RCLCPP_ERROR(m_node->get_logger(), "The received biases will be ignored.");
            rsp->success = false;
            rsp->status_message = "Failed to apply IMU biases since biases are already applied.";
            return false;
        }

        RCLCPP_INFO(m_node->get_logger(), "Setting IMU biases...");
        m_acc_biases = Eigen::Vector3f(req->acc_biases.x, req->acc_biases.y, req->acc_biases.z);
        m_gyro_biases = Eigen::Vector3f(req->gyro_biases.x, req->gyro_biases.y, req->gyro_biases.z);

        try
        {
            saveCalibration();
            rsp->success = true;
            rsp->status_message = "IMU biases stored in calibration file.";
            RCLCPP_INFO(m_node->get_logger(), "Unbiasing output IMU messages.");
            m_params.unbiasedImuData = true;
        }
        catch (const std::exception &e)
        {
            rsp->success = false;
            rsp->status_message = fmt::format("Error storing IMU biases calibration: [{}].", e.what());
        }
        return true;
    }

    bool Capture::setImuInfo(const std::shared_ptr<rmw_request_id_t> request_header,
                            const std::shared_ptr<dv_ros2_msgs::srv::SetImuInfo::Request> req,
                            std::shared_ptr<dv_ros2_msgs::srv::SetImuInfo::Response> rsp)
    {
        (void)request_header;
        m_imu_time_offset = req->imu_info.time_offset_micros;
        geometry_msgs::msg::TransformStamped stampedTransform;
        stampedTransform.transform = req->imu_info.t_sc;
        stampedTransform.header.frame_id = m_params.imuFrameName;
        stampedTransform.child_frame_id = m_params.cameraFrameName;
        m_imu_to_cam_transforms->transforms[0] = stampedTransform;

        Eigen::Quaternion<float> q(static_cast<float>(stampedTransform.transform.rotation.w),
                                   static_cast<float>(stampedTransform.transform.rotation.x),
                                   static_cast<float>(stampedTransform.transform.rotation.y),
                                   static_cast<float>(stampedTransform.transform.rotation.z));
        m_imu_to_cam_transform = dv::kinematics::Transformationf(0, Eigen::Vector3f::Zero(), q);

        try
        {
            auto calibPath = saveCalibration();
            rsp->success = true;
            rsp->status_message = fmt::format("IMU calibration stored successfully in [{}].", calibPath);
        }
        catch (const std::exception &e)
        {
            rsp->success = false;
            rsp->status_message = fmt::format("Error storing IMU info: [{}].", e.what());
        }
       
        return true;
    }

    fs::path Capture::getCameraCalibrationDirectory(const bool createDirectories) const
    {
        const fs::path directory = fmt::format("{0}/.dv_camera/camera_calibration/{1}", std::getenv("HOME"), m_reader.getCameraName());
        if (createDirectories && !fs::exists(directory))
        {
            fs::create_directories(directory);
        }
        return directory;
    }

    fs::path Capture::getActiveCalibrationPath() const
    {
        return getCameraCalibrationDirectory() / "active_calibration.json";
    }

    void Capture::generateActiveCalibrationFile()
    {
        RCLCPP_INFO(m_node->get_logger(), "Generating active calibration file...");
        updateCalibrationSet();
        m_calibration.writeToFile(getActiveCalibrationPath());
    }

    fs::path Capture::saveCalibration()
    {
        auto date = fmt::format("{:%Y_%m_%d_%H_%M_%S}", dv::toTimePoint(dv::now()));
        const std::string calibrationFileName = fmt::format("calibration_camera_{0}_{1}.json", m_reader.getCameraName(), date);
        const fs::path calibPath = getCameraCalibrationDirectory() / calibrationFileName;
        updateCalibrationSet();
        m_calibration.writeToFile(calibPath);

        fs::copy_file(calibPath, getActiveCalibrationPath(), fs::copy_options::overwrite_existing);
        return calibPath;
    }

    void Capture::updateNoiseFilter(const bool enable, const int64_t backgroundActivityTime)
    {
        if (enable)
        {
            // Create the filter and return
            if (m_noise_filter == nullptr)
            {
                m_noise_filter = std::make_unique<dv::noise::BackgroundActivityNoiseFilter<>>(m_reader.getEventResolution().value(), dv::Duration(backgroundActivityTime));
                return;
            }

            // Noise filter is instantiated, just update the period
            m_noise_filter->setBackgroundActivityDuration(dv::Duration(backgroundActivityTime));
        }
        else
        {
            // Destroy the filter
            m_noise_filter = nullptr;
        }
    }

    void Capture::sendSyncCalls(const std::map<std::string, std::string> &serviceNames) const
    {
        if (serviceNames.empty()) 
        {
            return;
        }

        const auto &liveCapture = m_reader.getCameraCapturePtr();
        if (!liveCapture) 
        {
            return;
        }

        auto request = std::make_shared<dv_ros2_msgs::srv::SynchronizeCamera::Request>();
        request->timestamp_offset = liveCapture->getTimestampOffset();
        request->master_camera_name = liveCapture->getCameraName();

        for (const auto &[cameraName, serviceName] : serviceNames) 
        {
            if (serviceName.empty())
            {
                RCLCPP_ERROR_STREAM(m_node->get_logger(), "Camera [" << cameraName
                                            << "] can't be synchronized, synchronization service "
                                            "is unavailable, please check synchronization cable!");
                continue;
            }

            rclcpp::Client<dv_ros2_msgs::srv::SynchronizeCamera>::SharedPtr client = m_node->create_client<dv_ros2_msgs::srv::SynchronizeCamera>(serviceName);
            client->wait_for_service(std::chrono::seconds(1));
            auto result = client->async_send_request(request);
            if (rclcpp::spin_until_future_complete(m_node, result) == rclcpp::FutureReturnCode::SUCCESS)
            {
                RCLCPP_INFO_STREAM(m_node->get_logger(), "Camera [" << cameraName << "] is synchronized.");
            }
            else
            {
                RCLCPP_ERROR_STREAM(m_node->get_logger(), "Device [" << cameraName
                                            << "] failed to synchronize on service [" << serviceName << "]");
            }
        }   
    }

    void Capture::synchronizeCamera(const std::shared_ptr<rmw_request_id_t> request_header, 
                                    const std::shared_ptr<dv_ros2_msgs::srv::SynchronizeCamera::Request> req,
                                    std::shared_ptr<dv_ros2_msgs::srv::SynchronizeCamera::Response> rsp)
    {
        (void)request_header;
        RCLCPP_INFO_STREAM(m_node->get_logger(), "Synchronization request received from [" << req->master_camera_name << "]");

        // assume failure case
        rsp->success = false;

        auto &liveCapture = m_reader.getCameraCapturePtr();
        if (!liveCapture) 
        {
            RCLCPP_WARN(m_node->get_logger(), "Received synchronization request on a non-live camera!");
            //return true;
        }
        if (liveCapture->isRunning() && liveCapture->isMasterCamera()) 
        {
            // Update the timestamp offset
            liveCapture->setTimestampOffset(req->timestamp_offset);
            RCLCPP_INFO_STREAM(m_node->get_logger(), "Camera [" << liveCapture->getCameraName() << "] synchronized: timestamp offset updated.");
            rsp->camera_name = liveCapture->getCameraName();
            rsp->success = true;
            m_synchronized = true;
        }
        else
        {
            RCLCPP_WARN(m_node->get_logger(), "Received synchronization request on a master camera, please check synchronization cable!");
        }
        //return true;
    }

    void Capture::synchronizationThread()
    {
        RCLCPP_INFO(m_node->get_logger(), "Spinning synchronization thread.");
        std::string serviceName;
        const auto &liveCapture = m_reader.getCameraCapturePtr();
        if (liveCapture->isMasterCamera()) 
        {
            RCLCPP_INFO_STREAM(m_node->get_logger(), "Camera [" << liveCapture->getCameraName() << "] is master camera.");
            // Wait for all cameras to show up
            const auto syncServiceList = discoverSyncDevices();
            runDiscovery(serviceName);
            sendSyncCalls(syncServiceList);
            m_synchronized = true;
        }
        else 
        {
            auto server_service = m_node->create_service<dv_ros2_msgs::srv::SynchronizeCamera>(fmt::format("{}/sync", liveCapture->getCameraName()), std::bind(&Capture::synchronizeCamera, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

            serviceName = server_service->get_service_name();
            runDiscovery(serviceName);

            // Wait for synchronization only if explicitly requested
            if (m_params.waitForSync)
            {
                m_synchronized = true;
            }

            size_t iterations = 0;
            while (m_spin_thread.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));

                // Do not print warnings if it's synchronized
                if (m_synchronized.load(std::memory_order_relaxed))
                {
                    continue;
                }

                if (iterations > 2000)
                {
                    RCLCPP_WARN(m_node->get_logger(), "[%s] waiting for synchronization service call...", liveCapture->getCameraName().c_str());
                    iterations = 0;
                }
                iterations++;
            }
        }

    }

    void Capture::framePublisher()
    {
        RCLCPP_INFO(m_node->get_logger(), "Spinning frame publisher.");
        
        std::optional<dv::Frame> frame = std::nullopt;

        while (m_spin_thread)
        {
            m_frame_queue.consume_all([&](const int64_t timestamp)
            {
                if (!frame.has_value())
                {
                    std::lock_guard<boost::recursive_mutex> lockGuard(m_reader_mutex);
                    frame = m_reader.getNextFrame();
                }
                while (frame.has_value() && timestamp >= frame->timestamp)
                {
                    if (m_frame_publisher->get_subscription_count() > 0)
                    {
                        auto msg = dv_ros2_msgs::frameToRosImageMessage(*frame);
                        m_frame_publisher->publish(msg);
                    }

                    m_current_seek = frame->timestamp;

                    std::lock_guard<boost::recursive_mutex> lockGuard(m_reader_mutex);
                    frame = m_reader.getNextFrame();
                }
            });
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void Capture::imuPublisher()
    {
        RCLCPP_INFO(m_node->get_logger(), "Spinning imu publisher.");

        std::optional<dv::cvector<dv::IMU>> imuData = std::nullopt;

        while(m_spin_thread)
        {
            m_imu_queue.consume_all([&](const int64_t timestamp)
            {
                if (!imuData.has_value())
                {
                    std::lock_guard<boost::recursive_mutex> lockGuard(m_reader_mutex);
                    imuData = m_reader.getNextImuBatch();
                }
                while (imuData.has_value() && !imuData->empty() && timestamp >= imuData->back().timestamp)
                {
                    if (m_imu_publisher->get_subscription_count() > 0)
                    {
                        for (auto &imu : *imuData)
                        {
                            imu.timestamp += m_imu_time_offset;
                            m_imu_publisher->publish(transformImuFrame(dv_ros2_msgs::toRosImuMessage(imu)));
                        }
                    }
                    m_current_seek = imuData->back().timestamp;

                    std::lock_guard<boost::recursive_mutex> lockGuard(m_reader_mutex);
                    imuData = m_reader.getNextImuBatch();
                }

                // If value present but empty, we don't want to keep it for later spins.
                if (imuData.has_value() && imuData->empty())
                {
                    imuData = std::nullopt;
                }
            });
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void Capture::eventsPublisher()
    {
        RCLCPP_INFO(m_node->get_logger(), "Spinning events publisher.");
        
        std::optional<dv::EventStore> events = std::nullopt;

        cv::Size resolution = m_reader.getEventResolution().value();

        while (m_spin_thread)
        {
            m_events_queue.consume_all([&](const int64_t timestamp)
            {
                if (!events.has_value())
                {
                    std::lock_guard<boost::recursive_mutex> lockGuard(m_reader_mutex);
                    events = m_reader.getNextEventBatch();
                }
                while (events.has_value() && !events->isEmpty() && timestamp >= events->getHighestTime()) 
                {
				    dv::EventStore store;
                    if (m_noise_filter != nullptr) 
                    {
                        m_noise_filter->accept(*events);
                        store = m_noise_filter->generateEvents();
                    }
                    else 
                    {
                        store = *events;
                    }

                    if (m_events_publisher->get_subscription_count() > 0) 
                    {
                        auto msg = dv_ros2_msgs::toRosEventsMessage(store, resolution);
                        m_events_publisher->publish(msg);
                    }
                    m_current_seek = store.getHighestTime();

                    std::lock_guard<boost::recursive_mutex> lockGuard(m_reader_mutex);
                    events = m_reader.getNextEventBatch();
                }

                if (events.has_value() && events->isEmpty()) 
                {
                    events = std::nullopt;
                }
            });
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void Capture::triggerPublisher()
    {
        RCLCPP_INFO(m_node->get_logger(), "Spinning trigger publisher.");

        std::optional<dv::cvector<dv::Trigger>> triggerData = std::nullopt;

        while (m_spin_thread)
        {
            m_trigger_queue.consume_all([&](const int64_t timestamp)
            {
                if (!triggerData.has_value())
                {
                    std::lock_guard<boost::recursive_mutex> lockGuard(m_reader_mutex);
                    triggerData = m_reader.getNextTriggerBatch();
                }
                while (triggerData.has_value() && !triggerData->empty() && timestamp >= triggerData->back().timestamp)
                {
                    if (m_trigger_publisher->get_subscription_count() > 0)
                    {
                        for (const auto &trigger : *triggerData)
                        {
                            m_trigger_publisher->publish(dv_ros2_msgs::toRosTriggerMessage(trigger));
                        }
                    }
                    m_current_seek = triggerData->back().timestamp;

                    std::lock_guard<boost::recursive_mutex> lockGuard(m_reader_mutex);
                    triggerData = m_reader.getNextTriggerBatch();
                }

                // If value present but empty, we don't want to keep it for later spins.
                if (triggerData.has_value() && triggerData->empty())
                {
                    triggerData = std::nullopt;
                }
            });
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}