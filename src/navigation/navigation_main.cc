//========================================================================
//  This software is free: you can redistribute it and/or modify
//  it under the terms of the GNU Lesser General Public License Version 3,
//  as published by the Free Software Foundation.
//
//  This software is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public License
//  Version 3 in the file COPYING that came with this distribution.
//  If not, see <http://www.gnu.org/licenses/>.
//========================================================================
/*!
\file    navigation_main.cc
\brief   Main entry point for reference Navigation implementation
\author  Joydeep Biswas, (C) 2019
*/
//========================================================================

#include <signal.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <vector>

#include "amrl_msgs/Localization2DMsg.h"
#include "config_reader/config_reader.h"
#include "cv_bridge/cv_bridge.h"
#include "glog/logging.h"
#include "gflags/gflags.h"
#include "eigen3/Eigen/Dense"
#include "eigen3/Eigen/Geometry"
#include "gflags/gflags.h"
#include "geometry_msgs/Pose2D.h"
#include "geometry_msgs/PoseArray.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/PoseWithCovarianceStamped.h"
#include "image_transport/image_transport.h"
#include "opencv2/core/mat.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "sensor_msgs/LaserScan.h"
#include "sensor_msgs/CompressedImage.h"
#include "sensor_msgs/image_encodings.h"
#include "visualization_msgs/Marker.h"
#include "visualization_msgs/MarkerArray.h"
#include "nav_msgs/Odometry.h"
#include "ros/ros.h"
#include "ros/package.h"
#include "shared/math/math_util.h"
#include "shared/util/timer.h"
#include "shared/util/helpers.h"
#include "shared/ros/ros_helpers.h"
#include "std_msgs/Bool.h"

#include "motion_primitives.h"
#include "navigation.h"

using math_util::DegToRad;
using math_util::RadToDeg;
using navigation::Navigation;
using ros::Time;
using ros_helpers::Eigen3DToRosPoint;
using ros_helpers::Eigen2DToRosPoint;
using ros_helpers::RosPoint;
using ros_helpers::SetRosVector;
using std::string;
using std::vector;
using Eigen::Vector2f;
using navigation::MotionLimits;

const string kAmrlMapsDir = ros::package::getPath("amrl_maps");
const string kOpenCVWindow = "Image window";

DEFINE_string(robot_config, "config/navigation.lua", "Robot config file");
DEFINE_string(maps_dir, kAmrlMapsDir, "Directory containing AMRL maps");

CONFIG_STRING(image_topic, "NavigationParameters.image_topic");
CONFIG_STRING(laser_topic, "NavigationParameters.laser_topic");
CONFIG_STRING(odom_topic, "NavigationParameters.odom_topic");
CONFIG_STRING(localization_topic, "NavigationParameters.localization_topic");
CONFIG_STRING(init_topic, "NavigationParameters.init_topic");
CONFIG_STRING(enable_topic, "NavigationParameters.enable_topic");
CONFIG_FLOAT(laser_loc_x, "NavigationParameters.laser_loc.x");
CONFIG_FLOAT(laser_loc_y, "NavigationParameters.laser_loc.y");

DEFINE_string(map, "UT_Campus", "Name of navigation map file");
DEFINE_bool(debug_images, false, "Show debug images");

DECLARE_int32(v);

bool run_ = true;
sensor_msgs::LaserScan last_laser_msg_;
Navigation navigation_;
cv::Mat last_image_;

void EnablerCallback(const std_msgs::Bool& msg) {
  navigation_.Enable(msg.data);
}

void LaserCallback(const sensor_msgs::LaserScan& msg) {
  if (FLAGS_v > 0) {
    printf("Laser t=%f, dt=%f\n",
           msg.header.stamp.toSec(),
           GetWallTime() - msg.header.stamp.toSec());
  }
  // Location of the laser on the robot. Assumes the laser is forward-facing.
  const Vector2f kLaserLoc(CONFIG_laser_loc_x, CONFIG_laser_loc_y);
  static float cached_dtheta_ = 0;
  static float cached_angle_min_ = 0;
  static size_t cached_num_rays_ = 0;
  static vector<Vector2f> cached_rays_;
  if (cached_angle_min_ != msg.angle_min ||
      cached_dtheta_ != msg.angle_increment ||
      cached_num_rays_ != msg.ranges.size()) {
    cached_angle_min_ = msg.angle_min;
    cached_dtheta_ = msg.angle_increment;
    cached_num_rays_ = msg.ranges.size();
    cached_rays_.resize(cached_num_rays_);
    for (size_t i = 0; i < cached_num_rays_; ++i) {
      const float a =
          cached_angle_min_ + static_cast<float>(i) * cached_dtheta_;
      cached_rays_[i] = Vector2f(cos(a), sin(a));
    }
  }
  CHECK_EQ(cached_rays_.size(), msg.ranges.size());
  static vector<Vector2f> point_cloud_;
  point_cloud_.resize(cached_rays_.size());
  for (size_t i = 0; i < cached_num_rays_; ++i) {
    const float r =
      ((msg.ranges[i] > msg.range_min && msg.ranges[i] < msg.range_max) ?
      msg.ranges[i] : msg.range_max);
    point_cloud_[i] = r * cached_rays_[i] + kLaserLoc;
  }
  navigation_.ObservePointCloud(point_cloud_, msg.header.stamp.toSec());
  last_laser_msg_ = msg;
}

void OdometryCallback(const nav_msgs::Odometry& msg) {
  if (FLAGS_v > 0) {
    printf("Odometry t=%f\n", msg.header.stamp.toSec());
  }
  navigation_.UpdateOdometry(msg);
}

void GoToCallback(const geometry_msgs::PoseStamped& msg) {
  const Vector2f loc(msg.pose.position.x, msg.pose.position.y);
  const float angle =
      2.0 * atan2(msg.pose.orientation.z, msg.pose.orientation.w);
  printf("Goal: (%f,%f) %f\u00b0\n", loc.x(), loc.y(), angle);
  navigation_.SetNavGoal(loc, angle);
}

void GoToCallbackAMRL(const amrl_msgs::Localization2DMsg& msg) {
  const Vector2f loc(msg.pose.x, msg.pose.y);
  printf("Goal: (%f,%f) %f\u00b0\n", loc.x(), loc.y(), msg.pose.theta);
  navigation_.SetNavGoal(loc, msg.pose.theta);
}

void QueryCallback(const geometry_msgs::PoseStamped& msg) {
  const Vector2f loc(msg.pose.position.x, msg.pose.position.y);
  printf("Queried Plan for Goal: (%f,%f)\n", loc.x(), loc.y());
  navigation_.Plan(loc);
}

void QueryCallbackAMRL(const amrl_msgs::Localization2DMsg& msg) {
  const Vector2f loc(msg.pose.x, msg.pose.y);
  printf("Queried Plan for Goal: (%f,%f)\n", loc.x(), loc.y());
  navigation_.Plan(loc);
}

void SignalHandler(int) {
  if (!run_) {
    printf("Force Exit.\n");
    exit(0);
  }
  printf("Exiting.\n");
  run_ = false;
}

void LocalizationCallback(const amrl_msgs::Localization2DMsg& msg) {
  static string map  = "";
  if (FLAGS_v > 0) {
    printf("Localization t=%f\n", GetWallTime());
  }
  navigation_.UpdateLocation(Vector2f(msg.pose.x, msg.pose.y), msg.pose.theta);
  if (map != msg.map) {
    map = msg.map;
    navigation_.UpdateMap(navigation::GetMapPath(FLAGS_maps_dir, msg.map));
  }
}

void HaltCallback(const std_msgs::Bool& msg) {
  navigation_.Abort();
}

void LoadConfig(navigation::NavigationParameters* params) {
  #define REAL_PARAM(x) CONFIG_DOUBLE(x, "NavigationParameters."#x);
  #define NATURALNUM_PARAM(x) CONFIG_UINT(x, "NavigationParameters."#x);
  #define STRING_PARAM(x) CONFIG_STRING(x, "NavigationParameters."#x);
  #define BOOL_PARAM(x) CONFIG_BOOL(x, "NavigationParameters."#x);
  REAL_PARAM(dt);
  REAL_PARAM(max_linear_accel);
  REAL_PARAM(max_linear_decel);
  REAL_PARAM(max_linear_speed);
  REAL_PARAM(max_angular_accel);
  REAL_PARAM(max_angular_decel);
  REAL_PARAM(max_angular_speed);
  REAL_PARAM(carrot_dist);
  REAL_PARAM(system_latency);
  REAL_PARAM(obstacle_margin);
  NATURALNUM_PARAM(num_options);
  REAL_PARAM(robot_width);
  REAL_PARAM(robot_length);
  REAL_PARAM(base_link_offset);
  REAL_PARAM(max_free_path_length);
  REAL_PARAM(max_clearance);
  BOOL_PARAM(can_traverse_stairs);
  REAL_PARAM(target_dist_tolerance);
  REAL_PARAM(target_vel_tolerance);
  BOOL_PARAM(use_kinect);
  STRING_PARAM(model_path);
  STRING_PARAM(embedding_model_path);
  STRING_PARAM(evaluator_type);
  BOOL_PARAM(blur);

  config_reader::ConfigReader reader({FLAGS_robot_config});
  params->dt = CONFIG_dt;
  params->linear_limits = MotionLimits(
      CONFIG_max_linear_accel,
      CONFIG_max_linear_decel,
      CONFIG_max_linear_speed);
  params->angular_limits = MotionLimits(
      CONFIG_max_angular_accel,
      CONFIG_max_angular_decel,
      CONFIG_max_angular_speed);
  params->carrot_dist = CONFIG_carrot_dist;
  params->system_latency = CONFIG_system_latency;
  params->obstacle_margin = CONFIG_obstacle_margin;
  params->num_options = CONFIG_num_options;
  params->robot_width = CONFIG_robot_width;
  params->robot_length = CONFIG_robot_length;
  params->base_link_offset = CONFIG_base_link_offset;
  params->max_free_path_length = CONFIG_max_free_path_length;
  params->max_clearance = CONFIG_max_clearance;
  params->can_traverse_stairs = CONFIG_can_traverse_stairs;
  params->target_dist_tolerance = CONFIG_target_dist_tolerance;
  params->target_vel_tolerance = CONFIG_target_vel_tolerance;
  params->use_kinect = CONFIG_use_kinect;
  params->model_path = CONFIG_model_path;
  params->embedding_model_path = CONFIG_embedding_model_path;
  params->evaluator_type = CONFIG_evaluator_type;
  params->blur = CONFIG_blur;

  if (params->use_kinect) {
    params->K = { 622.90532,   0.     , 639.44796, 0.     , 620.84752, 368.20234, 0.     ,   0.     ,   1.     };
    params->D = { 0.092890, -0.046208, 0.000622, -0.001104, 0.000000 };
    params->H.push_back({-0.5,-1.5,397,523});
    params->H.push_back({0.5,-1.5,832,515});
    params->H.push_back({0.5,-2.5,752,413});
    params->H.push_back({-0.5,-2.5,491,416});
  } else {
    params->K = { 867.04679,   0.     , 653.18207, 0.     , 866.39461, 537.77518, 0.     ,   0.     ,   1.};
    params->D = { -0.059124, 0.081963, 0.000743, 0.002461, 0.000000 };
    params->H.push_back({-0.5,-1.5,369,696});
    params->H.push_back({0.5,-1.5,971,687});
    params->H.push_back({0.5,-2.5,835,570});
    params->H.push_back({-0.5,-2.5,478,573});
  }
}

void ImageCallback(const sensor_msgs::CompressedImageConstPtr& msg) {
  try {
    cv_bridge::CvImagePtr image = cv_bridge::toCvCopy(
        msg, sensor_msgs::image_encodings::BGR8);
    last_image_ = image->image;
  } catch (cv_bridge::Exception& e) {
    fprintf(stderr, "cv_bridge exception: %s\n", e.what());
    return;
  }
  navigation_.ObserveImage(last_image_, msg->header.stamp.toSec());
  // Update GUI Window
  if (FLAGS_debug_images) {
    cv::imshow(kOpenCVWindow, last_image_);
    cv::waitKey(3);
  }
}

int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, false);
  google::InitGoogleLogging(argv[0]);
  signal(SIGINT, SignalHandler);
  // Initialize ROS.
  ros::init(argc, argv, "navigation", ros::init_options::NoSigintHandler);
  ros::NodeHandle n;
  std::string map_path = navigation::GetMapPath(FLAGS_maps_dir, FLAGS_map);
  std::string deprecated_path = navigation::GetDeprecatedMapPath(FLAGS_maps_dir, FLAGS_map);
  if (!FileExists(map_path) && FileExists(deprecated_path)) {
    printf("Could not find navigation map file at %s. An V1 nav-map was found at %s. Please run map_upgrade from vector_display to upgrade this map.\n", map_path.c_str(), deprecated_path.c_str());
    return 1;
  } else if (!FileExists(map_path)) {
    printf("Could not find navigation map file at %s.\n", map_path.c_str());
    return 1;
  }

  navigation::NavigationParameters params;
  LoadConfig(&params);

  navigation_.Initialize(params, map_path, &n);

  ros::Subscriber velocity_sub =
      n.subscribe(CONFIG_odom_topic, 1, &OdometryCallback);
  ros::Subscriber localization_sub =
      n.subscribe(CONFIG_localization_topic, 1, &LocalizationCallback);
  ros::Subscriber laser_sub =
      n.subscribe(CONFIG_laser_topic, 1, &LaserCallback);
  ros::Subscriber goto_sub =
      n.subscribe("/move_base_simple/goal", 1, &GoToCallback);
  ros::Subscriber goto_amrl_sub =
      n.subscribe("/move_base_simple/goal_amrl", 1, &GoToCallbackAMRL);
  ros::Subscriber query_sub =
      n.subscribe("/query_simple/goal", 1, &QueryCallback);
  ros::Subscriber query_amrl_sub =
      n.subscribe("/query_simple/goal_amrl", 1, &QueryCallbackAMRL);
  ros::Subscriber enabler_sub =
      n.subscribe(CONFIG_enable_topic, 1, &EnablerCallback);
  ros::Subscriber halt_sub =
      n.subscribe("halt_robot", 1, &HaltCallback);
  ros::Subscriber image_sub = n.subscribe(CONFIG_image_topic, 1, &ImageCallback);

  if (FLAGS_debug_images) cv::namedWindow(kOpenCVWindow);
  RateLoop loop(1.0 / params.dt);
  while (run_ && ros::ok()) {
    ros::spinOnce();
    navigation_.Run();
    loop.Sleep();
  }
  if (FLAGS_debug_images) cv::destroyWindow(kOpenCVWindow);
  return 0;
}
