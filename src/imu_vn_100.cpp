/*
 * Copyright [2015] [Ke Sun]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <imu_vn_100/imu_vn_100.h>

namespace imu_vn_100 {

using diagnostic_updater::FrequencyStatusParam;
using diagnostic_updater::TimeStampStatusParam;

void RosVector3FromVnVector3(geometry_msgs::Vector3& ros_vec3,
                             const VnVector3& vn_vec3);
void RosQuaternionFromVnQuaternion(geometry_msgs::Quaternion& ros_quat,
                                   const VnQuaternion& vn_quat);
void FillImuMessage(sensor_msgs::Imu& imu_msg,
                    const VnDeviceCompositeData& data, bool binary_output);

// LESS HACK IS STILL HACK
ImuVn100* evil_global_;

constexpr int ImuVn100::kBaseImuRate;
constexpr int ImuVn100::kDefaultImuRate;
constexpr int ImuVn100::kDefaultSyncOutRate;

void AsyncListener(void* sender, VnDeviceCompositeData* data) {
  evil_global_->PublishData(*data);
}

ImuVn100::ImuVn100(const ros::NodeHandle& pnh)
    : pnh_(pnh),
      port_(std::string("/dev/ttyUSB0")),
      baudrate_(921600),
      frame_id_(std::string("imu")),
      sync_out_pulse_width_us_(500000) {
  Initialize();
  evil_global_ = this;
}

ImuVn100::~ImuVn100() { Disconnect(); }

void ImuVn100::FixImuRate() {
  if (imu_rate_ <= 0) {
    ROS_WARN("Imu rate %d is < 0. Set to %d", imu_rate_, kDefaultImuRate);
    imu_rate_ = kDefaultImuRate;
  }

  if (kBaseImuRate % imu_rate_ != 0) {
    int imu_rate_old = imu_rate_;
    // TODO: THIS DOENS'T DO WHAT'S INTENDED
    imu_rate_ = kBaseImuRate / (kBaseImuRate / imu_rate_old);
    ROS_WARN("Imu rate %d cannot evenly decimate base rate %d, reset to %d",
             imu_rate_old, kBaseImuRate, imu_rate_);
  }
}

void ImuVn100::FixSyncOutRate() {
  // Check the sync out rate
  if (sync_out_rate_ > 0) {
    if (kBaseImuRate % sync_out_rate_ != 0) {
      sync_out_rate_ = 800.0 / (kBaseImuRate / sync_out_rate_);
      ROS_INFO("Set SYNC_OUT_RATE to %d", sync_out_rate_);
    }
    sync_out_skip_cnt_ = (std::floor(800.0 / sync_out_rate_ + 0.5f)) - 1;

    if (sync_out_pulse_width_us_ > 10000000) {
      ROS_INFO("Sync out pulse with is over 10ms. Reset to 0.5ms");
      sync_out_pulse_width_us_ = 500000;
    }
  }
}

void ImuVn100::LoadParameters() {
  pnh_.param<std::string>("port", port_, std::string("/dev/ttyUSB0"));
  pnh_.param<std::string>("frame_id", frame_id_, pnh_.getNamespace());
  pnh_.param("baudrate", baudrate_, 115200);
  pnh_.param("imu_rate", imu_rate_, kDefaultImuRate);

  pnh_.param("enable_mag", enable_mag_, true);
  pnh_.param("enable_pres", enable_pres_, true);
  pnh_.param("enable_temp", enable_temp_, true);

  pnh_.param("sync_out_rate", sync_out_rate_, kDefaultSyncOutRate);
  pnh_.param("sync_out_pulse_width", sync_out_pulse_width_us_, 500000);

  pnh_.param("binary_output", binary_output_, true);

  FixImuRate();
  FixSyncOutRate();
}

void ImuVn100::CreatePublishers() {
  // IMU data publisher
  pub_imu_ = pnh_.advertise<sensor_msgs::Imu>("imu", 1);
  if (enable_mag_) {
    pub_mag_ = pnh_.advertise<sensor_msgs::MagneticField>("magnetic_field", 1);
  }
  if (enable_pres_) {
    pub_pres_ = pnh_.advertise<sensor_msgs::FluidPressure>("pressure", 1);
  }
  if (enable_temp_) {
    pub_temp_ = pnh_.advertise<sensor_msgs::Temperature>("temperature", 1);
  }
}

void ImuVn100::CreateDiagnostics(const std::string& hardware_id) {
  updater_.setHardwareID(hardware_id);
  imu_rate_double_ = imu_rate_;
  FrequencyStatusParam freq_param(&imu_rate_double_, &imu_rate_double_, 0.01,
                                  10);
  TimeStampStatusParam time_param(0, 0.5 / imu_rate_double_);
  diag_imu_ = boost::make_shared<TopicDiagnostic>("imu", updater_, freq_param,
                                                  time_param);
  if (enable_mag_) {
    diag_mag_ = boost::make_shared<TopicDiagnostic>("magnetic_field", updater_,
                                                    freq_param, time_param);
  }
  if (enable_pres_) {
    diag_pres_ = boost::make_shared<TopicDiagnostic>("pressure", updater_,
                                                     freq_param, time_param);
  }
  if (enable_temp_) {
    diag_temp_ = boost::make_shared<TopicDiagnostic>("temperature", updater_,
                                                     freq_param, time_param);
  }
}

void ImuVn100::Resume(bool need_reply) {
  vn100_resumeAsyncOutputs(&imu_, need_reply);
}

void ImuVn100::Idle(bool need_reply) {
  vn100_pauseAsyncOutputs(&imu_, need_reply);
}

void ImuVn100::Disconnect() {
  vn100_reset(&imu_);
  vn100_disconnect(&imu_);
}

void ImuVn100::Initialize() {
  LoadParameters();

  ROS_DEBUG("Connecting to device");
  VnEnsure(vn100_connect(&imu_, port_.c_str(), 115200));
  ros::Duration(0.5).sleep();
  ROS_INFO("Connected to device at %s", port_.c_str());

  unsigned int old_baudrate;
  VnEnsure(vn100_getSerialBaudRate(&imu_, &old_baudrate));
  ROS_INFO("Default serial baudrate: %u", old_baudrate);

  ROS_INFO("Set serial baudrate to %d", baudrate_);
  VnEnsure(vn100_setSerialBaudRate(&imu_, baudrate_, true));

  ROS_DEBUG("Disconnecting the device");
  vn100_disconnect(&imu_);
  ros::Duration(0.5).sleep();

  ROS_DEBUG("Reconnecting to device");
  VnEnsure(vn100_connect(&imu_, port_.c_str(), baudrate_));
  ros::Duration(0.5).sleep();
  ROS_INFO("Connected to device at %s", port_.c_str());

  VnEnsure(vn100_getSerialBaudRate(&imu_, &old_baudrate));
  ROS_INFO("New serial baudrate: %u", old_baudrate);

  // Idle the device for intialization
  VnEnsure(vn100_pauseAsyncOutputs(&imu_, true));

  ROS_INFO("Fetching device info.");
  char model_number_buffer[30] = {0};
  int hardware_revision = 0;
  char serial_number_buffer[30] = {0};
  char firmware_version_buffer[30] = {0};

  VnEnsure(vn100_getModelNumber(&imu_, model_number_buffer, 30));
  ROS_INFO("Model number: %s", model_number_buffer);
  VnEnsure(vn100_getHardwareRevision(&imu_, &hardware_revision));
  ROS_INFO("Hardware revision: %d", hardware_revision);
  VnEnsure(vn100_getSerialNumber(&imu_, serial_number_buffer, 30));
  ROS_INFO("Serial number: %s", serial_number_buffer);
  VnEnsure(vn100_getFirmwareVersion(&imu_, firmware_version_buffer, 30));
  ROS_INFO("Firmware version: %s", firmware_version_buffer);

  if (sync_out_rate_ > 0) {
    ROS_INFO("Set Synchronization Control Register (id:32).");
    VnEnsure(vn100_setSynchronizationControl(
        &imu_, SYNCINMODE_COUNT, SYNCINEDGE_RISING, 0, SYNCOUTMODE_IMU_START,
        SYNCOUTPOLARITY_POSITIVE, sync_out_skip_cnt_, sync_out_pulse_width_us_,
        true));

    if (!binary_output_) {
      ROS_INFO("Set Communication Protocal Control Register (id:30).");
      VnEnsure(vn100_setCommunicationProtocolControl(
          &imu_, SERIALCOUNT_SYNCOUT_COUNT, SERIALSTATUS_OFF, SPICOUNT_NONE,
          SPISTATUS_OFF, SERIALCHECKSUM_8BIT, SPICHECKSUM_8BIT, ERRORMODE_SEND,
          true));
    }
  }

  auto hardware_id = std::string("vn100-") + std::string(model_number_buffer) +
                     std::string(serial_number_buffer);
  CreatePublishers();
  CreateDiagnostics(hardware_id);
}

void ImuVn100::Stream(bool async) {
  // Pause the device first
  VnEnsure(vn100_pauseAsyncOutputs(&imu_, true));

  if (async) {
    VnEnsure(vn100_setAsynchronousDataOutputType(&imu_, VNASYNC_OFF, true));

    if (binary_output_) {
      // Set the binary output data type and data rate
      VnEnsure(vn100_setBinaryOutput1Configuration(
          &imu_, BINARY_ASYNC_MODE_SERIAL_2, kBaseImuRate / imu_rate_,
          BG1_QTN | BG1_IMU | BG1_MAG_PRES | BG1_SYNC_IN_CNT,
          // BG1_IMU,
          BG3_NONE, BG5_NONE, true));
    } else {
      // Set the ASCII output data type and data rate
      // ROS_INFO("Configure the output data type and frequency (id: 6 & 7)");
      VnEnsure(vn100_setAsynchronousDataOutputType(&imu_, VNASYNC_VNIMU, true));
    }

    // Add a callback function for new data event
    VnEnsure(vn100_registerAsyncDataReceivedListener(&imu_, &AsyncListener));

    ROS_INFO("Setting IMU rate to %d", imu_rate_);
    VnEnsure(vn100_setAsynchronousDataOutputFrequency(&imu_, imu_rate_, true));
  } else {
    // Mute the stream
    ROS_DEBUG("Mute the device");
    VnEnsure(vn100_setAsynchronousDataOutputType(&imu_, VNASYNC_OFF, true));
    // Remove the callback function for new data event
    VnEnsure(vn100_unregisterAsyncDataReceivedListener(&imu_, &AsyncListener));
  }

  // Resume the device
  VnEnsure(vn100_resumeAsyncOutputs(&imu_, true));
}

void ImuVn100::PublishData(const VnDeviceCompositeData& data) {
  sensor_msgs::Imu imu_msg;
  imu_msg.header.stamp = ros::Time::now();
  imu_msg.header.frame_id = frame_id_;

  FillImuMessage(imu_msg, data, binary_output_);
  pub_imu_.publish(imu_msg);
  diag_imu_->tick(imu_msg.header.stamp);

  if (enable_mag_) {
    sensor_msgs::MagneticField mag_msg;
    mag_msg.header = imu_msg.header;
    RosVector3FromVnVector3(mag_msg.magnetic_field, data.magnetic);
    pub_mag_.publish(mag_msg);
    diag_mag_->tick(mag_msg.header.stamp);
  }

  if (enable_pres_) {
    sensor_msgs::FluidPressure pres_msg;
    pres_msg.header = imu_msg.header;
    pres_msg.fluid_pressure = data.pressure;
    pub_pres_.publish(pres_msg);
    diag_pres_->tick(pres_msg.header.stamp);
  }

  if (enable_temp_) {
    sensor_msgs::Temperature temp_msg;
    temp_msg.header = imu_msg.header;
    temp_msg.temperature = data.temperature;
    pub_temp_.publish(temp_msg);
    diag_temp_->tick(temp_msg.header.stamp);
  }

  if (sync_out_rate_ > 0) {
    sync_info.Update(data.syncInCnt, imu_msg.header.stamp);
  }

  updater_.update();
}

void VnEnsure(const VnErrorCode& error_code) {
  if (error_code == VNERR_NO_ERROR) return;

  switch (error_code) {
    case VNERR_UNKNOWN_ERROR:
      throw std::runtime_error("VN: Unknown error");
    case VNERR_NOT_IMPLEMENTED:
      throw std::runtime_error("VN: Not implemented");
    case VNERR_TIMEOUT:
      ROS_WARN("Opertation time out");
      break;
    case VNERR_SENSOR_INVALID_PARAMETER:
      ROS_WARN("VN: Sensor invalid paramter");
      break;
    case VNERR_INVALID_VALUE:
      ROS_WARN("VN: Invalid value");
      break;
    case VNERR_FILE_NOT_FOUND:
      ROS_WARN("VN: File not found");
      break;
    case VNERR_NOT_CONNECTED:
      throw std::runtime_error("VN: not connected");
    case VNERR_PERMISSION_DENIED:
      throw std::runtime_error("VN: Permission denied");
    default:
      ROS_WARN("We give no fuck");
  }
}

void RosVector3FromVnVector3(geometry_msgs::Vector3& ros_vec3,
                             const VnVector3& vn_vec3) {
  ros_vec3.x = vn_vec3.c0;
  ros_vec3.y = vn_vec3.c1;
  ros_vec3.z = vn_vec3.c2;
}

void RosQuaternionFromVnQuaternion(geometry_msgs::Quaternion& ros_quat,
                                   const VnQuaternion& vn_quat) {
  ros_quat.x = vn_quat.x;
  ros_quat.y = vn_quat.y;
  ros_quat.z = vn_quat.z;
  ros_quat.w = vn_quat.w;
}

void FillImuMessage(sensor_msgs::Imu& imu_msg,
                    const VnDeviceCompositeData& data, bool binary_output) {
  if (binary_output) {
    RosQuaternionFromVnQuaternion(imu_msg.orientation, data.quaternion);
    // NOTE: The IMU angular velocity and linear acceleration outputs are
    // swapped. And also why are they different?
    RosVector3FromVnVector3(imu_msg.angular_velocity,
                            data.accelerationUncompensated);
    RosVector3FromVnVector3(imu_msg.linear_acceleration,
                            data.angularRateUncompensated);
  } else {
    RosVector3FromVnVector3(imu_msg.linear_acceleration, data.acceleration);
    RosVector3FromVnVector3(imu_msg.angular_velocity, data.angularRate);
  }
}

}  //  namespace imu_vn_100
