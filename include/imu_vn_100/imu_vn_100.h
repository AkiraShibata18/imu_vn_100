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

#ifndef IMU_VN_100_ROS_H_
#define IMU_VN_100_ROS_H_

#include <ros/ros.h>
#include <diagnostic_updater/diagnostic_updater.h>
#include <diagnostic_updater/publisher.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/MagneticField.h>
#include <sensor_msgs/FluidPressure.h>
#include <sensor_msgs/Temperature.h>

#include <vn100.h>

namespace imu_vn_100 {

struct SyncInfo {
  unsigned sync_count;
  ros::Time sync_time;

  SyncInfo() : sync_count(-1), sync_time(ros::Time::now()) {}

  void Update(const unsigned sync_in_cnt, const ros::Time& time) {
    sync_count = sync_in_cnt;
    sync_time = time;
    if (sync_count == -1) {
      // Initialize the count if never set
      sync_count = sync_in_cnt;
      sync_time = time;
    } else {
      // Record the time when the sync counter increases
      if (sync_count != sync_in_cnt) {
        sync_count = sync_in_cnt;
        sync_time = time;
      }
    }
  }
};

using diagnostic_updater::TopicDiagnostic;
using TopicDiagnosticPtr = boost::shared_ptr<TopicDiagnostic>;
/**
 * @brief ImuRosBase The class is a ros wrapper for the Imu class
 * @author Ke Sun
 */
class ImuVn100 {
 public:
  explicit ImuVn100(const ros::NodeHandle& pnh);
  ImuVn100(const ImuVn100&) = delete;
  ImuVn100& operator=(const ImuVn100&) = delete;
  ~ImuVn100();

  void Initialize();

  void Stream(bool async = true);

  void PublishData(const VnDeviceCompositeData& data);

  void RequestOnce();

  void Idle(bool need_reply = true);

  void Resume(bool need_reply = true);

  void Disconnect();

  void Configure();

  int SyncOutRate() const;

  const ros::Time SyncTime() const;

 private:
  static constexpr int kBaseImuRate = 800;
  static constexpr int kDefaultImuRate = 100;
  static constexpr int kDefaultSyncOutRate = 20;

  ros::NodeHandle pnh_;
  Vn100 imu_;

  // Settings
  std::string port_;
  int baudrate_ = 921600;
  int imu_rate_ = kDefaultImuRate;
  double imu_rate_double_ = kDefaultImuRate;
  std::string frame_id_;

  bool enable_mag_ = true;
  bool enable_pres_ = true;
  bool enable_temp_ = true;
  bool binary_output_ = true;

  int sync_out_rate_ = kDefaultSyncOutRate;
  int sync_out_pulse_width_us_;
  int sync_out_skip_cnt_;

  // Tracking the triggering signal
  SyncInfo sync_info;

  ros::Publisher pub_imu_, pub_mag_, pub_pres_, pub_temp_;
  diagnostic_updater::Updater updater_;
  TopicDiagnosticPtr diag_imu_, diag_mag_, diag_pres_, diag_temp_;

  void FixImuRate();
  void FixSyncOutRate();

  void LoadParameters();
  void CreatePublishers();
  void CreateDiagnostics(const std::string& hardware_id);
};

// Just don't like type that is ALL CAP
using VnErrorCode = VN_ERROR_CODE;
void VnEnsure(const VnErrorCode& error_code);

}  // namespace imu_vn_100

#endif  // IMU_VN_100_ROS_H_
