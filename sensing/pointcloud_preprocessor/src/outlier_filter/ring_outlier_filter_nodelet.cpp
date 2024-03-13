// Copyright 2020 Tier IV, Inc.
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

#include "pointcloud_preprocessor/outlier_filter/ring_outlier_filter_nodelet.hpp"

#include "autoware_auto_geometry/common_3d.hpp"
#include "autoware_point_types/types.hpp"

#include <sensor_msgs/point_cloud2_iterator.hpp>

#include <pcl/search/pcl_search.h>

#include <algorithm>
#include <vector>
namespace pointcloud_preprocessor
{
using autoware_point_types::PointXYZIRADRT;

RingOutlierFilterComponent::RingOutlierFilterComponent(const rclcpp::NodeOptions & options)
: Filter("RingOutlierFilter", options)
{
  // initialize debug tool
  {
    using tier4_autoware_utils::DebugPublisher;
    using tier4_autoware_utils::StopWatch;
    stop_watch_ptr_ = std::make_unique<StopWatch<std::chrono::milliseconds>>();
    debug_publisher_ = std::make_unique<DebugPublisher>(this, "ring_outlier_filter");
    noise_points_publisher_ = this->create_publisher<PointCloud2>("noise/ring_outlier_filter", 1);
    image_pub_ =
      image_transport::create_publisher(this, "ring_outlier_filter/debug/frequency_image");
    visibility_pub_ = create_publisher<tier4_debug_msgs::msg::Float32Stamped>(
      "ring_outlier_filter/debug/visibility", rclcpp::SensorDataQoS());
    stop_watch_ptr_->tic("cyclic_time");
    stop_watch_ptr_->tic("processing_time");
  }

  // set initial parameters
  {
    distance_ratio_ = static_cast<double>(declare_parameter("distance_ratio", 1.03));
    object_length_threshold_ =
      static_cast<double>(declare_parameter("object_length_threshold", 0.1));
    num_points_threshold_ = static_cast<int>(declare_parameter("num_points_threshold", 4));
    max_rings_num_ = static_cast<uint16_t>(declare_parameter("max_rings_num", 128));
    max_points_num_per_ring_ =
      static_cast<size_t>(declare_parameter("max_points_num_per_ring", 4000));
    publish_noise_points_ = static_cast<bool>(declare_parameter("publish_noise_points", false));
    x_max_ = static_cast<float>(declare_parameter("x_max", 18.0));
    x_min_ = static_cast<float>(declare_parameter("x_min", -12.0));
    y_max_ = static_cast<float>(declare_parameter("y_max", 2.0));
    y_min_ = static_cast<float>(declare_parameter("y_min", -2.0));
    z_max_ = static_cast<float>(declare_parameter("z_max", 10.0));
    z_min_ = static_cast<float>(declare_parameter("z_min", 0.0));

    min_azimuth_deg_ = static_cast<float>(declare_parameter("min_azimuth_deg", 135.0));
    max_azimuth_deg_ = static_cast<float>(declare_parameter("max_azimuth_deg", 225.0));
    max_distance_ = static_cast<float>(declare_parameter("max_distance", 12.0));
    vertical_bins_ = static_cast<int>(declare_parameter("vertical_bins", 128));
    max_azimuth_diff_ = static_cast<float>(declare_parameter("max_azimuth_diff", 50.0));
    noise_threshold_ = static_cast<int>(declare_parameter("noise_threshold", 2));

    roi_mode_ = static_cast<std::string>(declare_parameter("roi_mode", "Fixed_xyz_ROI"));
  }

  using std::placeholders::_1;
  set_param_res_ = this->add_on_set_parameters_callback(
    std::bind(&RingOutlierFilterComponent::paramCallback, this, _1));
}

// TODO(sykwer): Temporary Implementation: Rename this function to `filter()` when all the filter
// nodes conform to new API. Then delete the old `filter()` defined below.
void RingOutlierFilterComponent::faster_filter(
  const PointCloud2ConstPtr & input, const IndicesPtr & unused_indices, PointCloud2 & output,
  const TransformInfo & transform_info)
{
  std::scoped_lock lock(mutex_);
  if (unused_indices) {
    RCLCPP_WARN(get_logger(), "Indices are not supported and will be ignored");
  }
  stop_watch_ptr_->toc("processing_time", true);

  output.point_step = sizeof(PointXYZI);
  output.data.resize(output.point_step * input->width);
  size_t output_size = 0;

  // Set up the noise points cloud, if noise points are to be published.
  PointCloud2 noise_points;
  size_t noise_points_size = 0;
  if (publish_noise_points_) {
    noise_points.point_step = sizeof(PointXYZI);
    noise_points.data.resize(noise_points.point_step * input->width);
  }

  const auto ring_offset =
    input->fields.at(static_cast<size_t>(autoware_point_types::PointIndex::Ring)).offset;
  const auto azimuth_offset =
    input->fields.at(static_cast<size_t>(autoware_point_types::PointIndex::Azimuth)).offset;
  const auto distance_offset =
    input->fields.at(static_cast<size_t>(autoware_point_types::PointIndex::Distance)).offset;
  const auto intensity_offset =
    input->fields.at(static_cast<size_t>(autoware_point_types::PointIndex::Intensity)).offset;

  std::vector<std::vector<size_t>> ring2indices;
  ring2indices.reserve(max_rings_num_);

  for (uint16_t i = 0; i < max_rings_num_; i++) {
    ring2indices.push_back(std::vector<size_t>());
    ring2indices.back().reserve(max_points_num_per_ring_);
  }

  for (size_t data_idx = 0; data_idx < input->data.size(); data_idx += input->point_step) {
    const uint16_t ring = *reinterpret_cast<const uint16_t *>(&input->data[data_idx + ring_offset]);
    ring2indices[ring].push_back(data_idx);
  }

  // walk range: [walk_first_idx, walk_last_idx]
  int walk_first_idx = 0;
  int walk_last_idx = -1;

  for (const auto & indices : ring2indices) {
    if (indices.size() < 2) continue;

    walk_first_idx = 0;
    walk_last_idx = -1;

    for (size_t idx = 0U; idx < indices.size() - 1; ++idx) {
      const size_t & current_data_idx = indices[idx];
      const size_t & next_data_idx = indices[idx + 1];
      walk_last_idx = idx;

      // if(std::abs(iter->distance - (iter+1)->distance) <= std::sqrt(iter->distance) * 0.08)

      const float & current_azimuth =
        *reinterpret_cast<const float *>(&input->data[current_data_idx + azimuth_offset]);
      const float & next_azimuth =
        *reinterpret_cast<const float *>(&input->data[next_data_idx + azimuth_offset]);
      float azimuth_diff = next_azimuth - current_azimuth;
      azimuth_diff = azimuth_diff < 0.f ? azimuth_diff + 36000.f : azimuth_diff;

      const float & current_distance =
        *reinterpret_cast<const float *>(&input->data[current_data_idx + distance_offset]);
      const float & next_distance =
        *reinterpret_cast<const float *>(&input->data[next_data_idx + distance_offset]);

      if (
        std::max(current_distance, next_distance) <
          std::min(current_distance, next_distance) * distance_ratio_ &&
        azimuth_diff < 100.f) {
        continue;  // Determined to be included in the same walk
      }

      if (isCluster(
            input, std::make_pair(indices[walk_first_idx], indices[walk_last_idx]),
            walk_last_idx - walk_first_idx + 1)) {
        for (int i = walk_first_idx; i <= walk_last_idx; i++) {
          auto output_ptr = reinterpret_cast<PointXYZI *>(&output.data[output_size]);
          auto input_ptr = reinterpret_cast<const PointXYZI *>(&input->data[indices[i]]);

          if (transform_info.need_transform) {
            Eigen::Vector4f p(input_ptr->x, input_ptr->y, input_ptr->z, 1);
            p = transform_info.eigen_transform * p;
            output_ptr->x = p[0];
            output_ptr->y = p[1];
            output_ptr->z = p[2];
          } else {
            *output_ptr = *input_ptr;
          }
          const float & intensity =
            *reinterpret_cast<const float *>(&input->data[indices[i] + intensity_offset]);
          output_ptr->intensity = intensity;

          output_size += output.point_step;
        }
      } else if (publish_noise_points_) {
        for (int i = walk_first_idx; i <= walk_last_idx; i++) {
          auto noise_ptr = reinterpret_cast<PointXYZI *>(&noise_points.data[noise_points_size]);
          auto input_ptr =
            reinterpret_cast<const PointXYZI *>(&input->data[indices[walk_first_idx]]);
          if (transform_info.need_transform) {
            Eigen::Vector4f p(input_ptr->x, input_ptr->y, input_ptr->z, 1);
            p = transform_info.eigen_transform * p;
            noise_ptr->x = p[0];
            noise_ptr->y = p[1];
            noise_ptr->z = p[2];
          } else {
            *noise_ptr = *input_ptr;
          }
          const float & intensity = *reinterpret_cast<const float *>(
            &input->data[indices[walk_first_idx] + intensity_offset]);
          noise_ptr->intensity = intensity;

          noise_points_size += noise_points.point_step;
        }
      }

      walk_first_idx = idx + 1;
    }

    if (walk_first_idx > walk_last_idx) continue;

    if (isCluster(
          input, std::make_pair(indices[walk_first_idx], indices[walk_last_idx]),
          walk_last_idx - walk_first_idx + 1)) {
      for (int i = walk_first_idx; i <= walk_last_idx; i++) {
        auto output_ptr = reinterpret_cast<PointXYZI *>(&output.data[output_size]);
        auto input_ptr = reinterpret_cast<const PointXYZI *>(&input->data[indices[i]]);

        if (transform_info.need_transform) {
          Eigen::Vector4f p(input_ptr->x, input_ptr->y, input_ptr->z, 1);
          p = transform_info.eigen_transform * p;
          output_ptr->x = p[0];
          output_ptr->y = p[1];
          output_ptr->z = p[2];
        } else {
          *output_ptr = *input_ptr;
        }
        const float & intensity =
          *reinterpret_cast<const float *>(&input->data[indices[i] + intensity_offset]);
        output_ptr->intensity = intensity;

        output_size += output.point_step;
      }
    } else if (publish_noise_points_) {
      for (int i = walk_first_idx; i < walk_last_idx; i++) {
        auto noise_ptr = reinterpret_cast<PointXYZI *>(&noise_points.data[noise_points_size]);
        auto input_ptr = reinterpret_cast<const PointXYZI *>(&input->data[indices[i]]);
        if (transform_info.need_transform) {
          Eigen::Vector4f p(input_ptr->x, input_ptr->y, input_ptr->z, 1);
          p = transform_info.eigen_transform * p;
          noise_ptr->x = p[0];
          noise_ptr->y = p[1];
          noise_ptr->z = p[2];
        } else {
          *noise_ptr = *input_ptr;
        }
        const float & intensity =
          *reinterpret_cast<const float *>(&input->data[indices[i] + intensity_offset]);
        noise_ptr->intensity = intensity;
        noise_points_size += noise_points.point_step;
      }
    }
  }

  setUpPointCloudFormat(input, output, output_size, /*num_fields=*/4);

  if (publish_noise_points_) {
    setUpPointCloudFormat(input, noise_points, noise_points_size, /*num_fields=*/4);
    noise_points_publisher_->publish(noise_points);

    const auto frequency_image = createBinaryImage(*input);
    const double num_filled_pixels = calculateFilledPixels(frequency_image, vertical_bins_, 36);
    std::cerr << "num_filled_pixels: " << num_filled_pixels << std::endl;
    std::cerr << "filled pixel rate: " << 1.0 - num_filled_pixels << std::endl;
    auto frequency_image_msg = toFrequencyImageMsg(frequency_image);

    frequency_image_msg.header = input->header;
    // Publish histogram image
    image_pub_.publish(frequency_image_msg);

    tier4_debug_msgs::msg::Float32Stamped visibility_msg;
    visibility_msg.data = (1.0f - num_filled_pixels);
    visibility_msg.stamp = input->header.stamp;
    visibility_pub_->publish(visibility_msg);
  }

  // add processing time for debug
  if (debug_publisher_) {
    const double cyclic_time_ms = stop_watch_ptr_->toc("cyclic_time", true);
    const double processing_time_ms = stop_watch_ptr_->toc("processing_time", true);
    debug_publisher_->publish<tier4_debug_msgs::msg::Float64Stamped>(
      "debug/cyclic_time_ms", cyclic_time_ms);
    debug_publisher_->publish<tier4_debug_msgs::msg::Float64Stamped>(
      "debug/processing_time_ms", processing_time_ms);

    auto pipeline_latency_ms =
      std::chrono::duration<double, std::milli>(
        std::chrono::nanoseconds((this->get_clock()->now() - input->header.stamp).nanoseconds()))
        .count();

    debug_publisher_->publish<tier4_debug_msgs::msg::Float64Stamped>(
      "debug/pipeline_latency_ms", pipeline_latency_ms);
  }
}

// TODO(sykwer): Temporary Implementation: Delete this function definition when all the filter
// nodes conform to new API
void RingOutlierFilterComponent::filter(
  const PointCloud2ConstPtr & input, [[maybe_unused]] const IndicesPtr & indices,
  PointCloud2 & output)
{
  (void)input;
  (void)indices;
  (void)output;
}

rcl_interfaces::msg::SetParametersResult RingOutlierFilterComponent::paramCallback(
  const std::vector<rclcpp::Parameter> & p)
{
  std::scoped_lock lock(mutex_);

  if (get_param(p, "distance_ratio", distance_ratio_)) {
    RCLCPP_DEBUG(get_logger(), "Setting new distance ratio to: %f.", distance_ratio_);
  }
  if (get_param(p, "object_length_threshold", object_length_threshold_)) {
    RCLCPP_DEBUG(
      get_logger(), "Setting new object length threshold to: %f.", object_length_threshold_);
  }
  if (get_param(p, "num_points_threshold", num_points_threshold_)) {
    RCLCPP_DEBUG(get_logger(), "Setting new num_points_threshold to: %d.", num_points_threshold_);
  }
  if (get_param(p, "publish_noise_points", publish_noise_points_)) {
    RCLCPP_DEBUG(get_logger(), "Setting new publish_noise_points to: %d.", publish_noise_points_);
  }
  if (get_param(p, "vertical_bins", vertical_bins_)) {
    RCLCPP_DEBUG(get_logger(), "Setting new vertical_bins to: %d.", vertical_bins_);
  }
  if (get_param(p, "max_azimuth_diff", max_azimuth_diff_)) {
    RCLCPP_DEBUG(get_logger(), "Setting new max_azimuth_diff to: %f.", max_azimuth_diff_);
  }

  rcl_interfaces::msg::SetParametersResult result;
  result.successful = true;
  result.reason = "success";

  return result;
}

void RingOutlierFilterComponent::setUpPointCloudFormat(
  const PointCloud2ConstPtr & input, PointCloud2 & formatted_points, size_t points_size,
  size_t num_fields)
{
  formatted_points.data.resize(points_size);
  // Note that `input->header.frame_id` is data before converted when
  // `transform_info.need_transform
  // == true`
  formatted_points.header.frame_id =
    !tf_input_frame_.empty() ? tf_input_frame_ : tf_input_orig_frame_;
  formatted_points.data.resize(formatted_points.point_step * input->width);
  formatted_points.height = 1;
  formatted_points.width =
    static_cast<uint32_t>(formatted_points.data.size() / formatted_points.point_step);
  formatted_points.is_bigendian = input->is_bigendian;
  formatted_points.is_dense = input->is_dense;

  sensor_msgs::PointCloud2Modifier pcd_modifier(formatted_points);
  pcd_modifier.setPointCloud2Fields(
    num_fields, "x", 1, sensor_msgs::msg::PointField::FLOAT32, "y", 1,
    sensor_msgs::msg::PointField::FLOAT32, "z", 1, sensor_msgs::msg::PointField::FLOAT32,
    "intensity", 1, sensor_msgs::msg::PointField::FLOAT32);
}

cv::Mat RingOutlierFilterComponent::createBinaryImage(const sensor_msgs::msg::PointCloud2 & input)
{
  pcl::PointCloud<PointXYZIRADRT>::Ptr input_cloud(new pcl::PointCloud<PointXYZIRADRT>);
  std::cerr << "before fromROSMsg at " << __LINE__ << std::endl;
  pcl::fromROSMsg(input, *input_cloud);
  std::cerr << "after fromROSMsg at  " << __LINE__ << std::endl;

  uint32_t vertical_bins = vertical_bins_;
  uint32_t horizontal_bins = 36;
  float max_azimuth = 36000.0f;
  float min_azimuth = 0.0f;

  std::cerr << "before switch-case at " << __LINE__ << std::endl;
  switch (roi_mode_map_[roi_mode_]) {
    case 2: {
      max_azimuth = max_azimuth_deg_ * 100.0;
      min_azimuth = min_azimuth_deg_ * 100.0;
      break;
    }

    default: {
      max_azimuth = 36000.0f;
      min_azimuth = 0.0f;
      break;
    }
  }
  std::cerr << "after switch-case" << roi_mode_map_[roi_mode_] << "at " << __LINE__ << std::endl;

  uint32_t horizontal_resolution =
    static_cast<uint32_t>((max_azimuth - min_azimuth) / horizontal_bins);

  std::vector<pcl::PointCloud<PointXYZIRADRT>> pcl_noise_ring_array;
  pcl_noise_ring_array.resize(vertical_bins);

  cv::Mat frequency_image(cv::Size(horizontal_bins, vertical_bins), CV_8UC1, cv::Scalar(0));

  std::cerr << "before processing points at " << __LINE__ << std::endl;
  // Split into 36 x 10 degree bins x 40 lines (TODO: change to dynamic)
  for (const auto & p : input_cloud->points) {
    pcl_noise_ring_array.at(p.ring).push_back(p);
  }
  std::cerr << "after processing points at " << __LINE__ << std::endl;

  std::cerr << "before analyzing segments at " << __LINE__ << std::endl;
  for (const auto & single_ring : pcl_noise_ring_array) {
    if (single_ring.points.empty()) {
      std::cerr << "Skipping empty ring" << std::endl;
      continue;
    }

    uint ring_id = single_ring.points.front().ring;
    std::vector<int> noise_frequency(horizontal_bins, 0);
    uint current_temp_segment_index = 0;

    std::cerr << "Analyzing ring: " << ring_id << " with " << single_ring.points.size()
              << " points." << std::endl;

    for (uint i = 0; i < noise_frequency.size() - 1; i++) {
      std::cerr << "Segment " << i << ": starting temp segment index " << current_temp_segment_index
                << std::endl;

      bool condition = true;
      while (condition && current_temp_segment_index < (single_ring.points.size() - 1)) {
        condition = (single_ring.points[current_temp_segment_index].azimuth < 0.f
                       ? 0.f
                       : single_ring.points[current_temp_segment_index].azimuth) <
                    ((i + 1 + static_cast<uint>(min_azimuth / horizontal_resolution)) *
                     horizontal_resolution);

        if (condition) {
          switch (roi_mode_map_[roi_mode_]) {
            case 1: {
              if (
                single_ring.points[current_temp_segment_index].x < x_max_ &&
                single_ring.points[current_temp_segment_index].x > x_min_ &&
                single_ring.points[current_temp_segment_index].y > y_max_ &&
                single_ring.points[current_temp_segment_index].y < y_min_ &&
                single_ring.points[current_temp_segment_index].z < z_max_ &&
                single_ring.points[current_temp_segment_index].z > z_min_) {
                noise_frequency[i] = noise_frequency[i] + 1;
              }
              break;
            }
            case 2: {
              if (
                single_ring.points[current_temp_segment_index].azimuth < max_azimuth &&
                single_ring.points[current_temp_segment_index].azimuth > min_azimuth &&
                single_ring.points[current_temp_segment_index].distance < max_distance_) {
                noise_frequency[i] = noise_frequency[i] + 1;
              }
              break;
            }
            default: {
              noise_frequency[i] = noise_frequency[i] + 1;
              break;
            }
          }

          std::cerr << "current_temp_segment_index: " << current_temp_segment_index
                    << ", azimuth: " << single_ring.points[current_temp_segment_index].azimuth
                    << ", condition: "
                    << ((i + 1 + static_cast<uint>(min_azimuth / horizontal_resolution)) *
                        horizontal_resolution)
                    << std::endl;
        }

        current_temp_segment_index++;
      }

      noise_frequency[i] =
        std::min(noise_frequency[i], 255);  // Ensure the value is within uchar range.
      frequency_image.at<uchar>(ring_id, i) = static_cast<uchar>(noise_frequency[i]);

      std::cerr << "Segment " << i << ": completed with noise frequency " << noise_frequency[i]
                << std::endl;
    }
  }

  std::cerr << "after analyzing segments at " << __LINE__ << std::endl;
  // Threshold for diagnostics (tunable)
  cv::Mat binary_image;
  cv::inRange(frequency_image, noise_threshold_, 255, binary_image);
  return binary_image;
}

float RingOutlierFilterComponent::calculateFilledPixels(
  const cv::Mat & frequency_image, const uint32_t vertical_bins, const uint32_t horizontal_bins)
{
  int num_pixels = cv::countNonZero(frequency_image);
  float num_filled_pixels =
    static_cast<float>(num_pixels) / static_cast<float>(vertical_bins * horizontal_bins);
  return num_filled_pixels;
}

sensor_msgs::msg::Image RingOutlierFilterComponent::toFrequencyImageMsg(
  const cv::Mat & frequency_image)
{
  cv_bridge::CvImage(std_msgs::msg::Header(), "mono8", frequency_image).toImageMsg();
  // Visualization of histogram
  cv::Mat frequency_image_colorized;
  // Multiply bins by four to get pretty colours
  cv::applyColorMap(frequency_image * 4, frequency_image_colorized, cv::COLORMAP_JET);
  sensor_msgs::msg::Image::SharedPtr frequency_image_msg =
    cv_bridge::CvImage(std_msgs::msg::Header(), "bgr8", frequency_image_colorized).toImageMsg();
  return *frequency_image_msg;
}

}  // namespace pointcloud_preprocessor

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(pointcloud_preprocessor::RingOutlierFilterComponent)
