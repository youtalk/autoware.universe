// Copyright 2019 Autoware Foundation
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

#include "mission_planner.hpp"

#include <lanelet2_extension/utility/message_conversion.hpp>
#include <lanelet2_extension/utility/query.hpp>
#include <lanelet2_extension/utility/route_checker.hpp>
#include <lanelet2_extension/utility/utilities.hpp>

#include <autoware_adapi_v1_msgs/srv/set_route.hpp>
#include <autoware_adapi_v1_msgs/srv/set_route_points.hpp>
#include <autoware_auto_mapping_msgs/msg/had_map_bin.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>

#include <algorithm>
#include <array>
#include <random>

namespace
{

using autoware_planning_msgs::msg::LaneletPrimitive;
using autoware_planning_msgs::msg::LaneletSegment;

LaneletPrimitive convert(const LaneletPrimitive & p)
{
  LaneletPrimitive primitive;
  primitive.id = p.id;
  primitive.primitive_type = p.primitive_type;
  return primitive;
}

LaneletSegment convert(const LaneletSegment & s)
{
  LaneletSegment segment;
  segment.preferred_primitive.id = s.preferred_primitive.id;
  segment.primitives.push_back(convert(s.preferred_primitive));
  for (const auto & p : s.primitives) {
    segment.primitives.push_back(convert(p));
  }
  return segment;
}

std::array<uint8_t, 16> generate_random_id()
{
  static std::independent_bits_engine<std::mt19937, 8, uint8_t> engine(std::random_device{}());
  std::array<uint8_t, 16> id;
  std::generate(id.begin(), id.end(), std::ref(engine));
  return id;
}

}  // namespace

namespace mission_planner
{

MissionPlanner::MissionPlanner(const rclcpp::NodeOptions & options)
: Node("mission_planner", options),
  arrival_checker_(this),
  plugin_loader_("mission_planner", "mission_planner::PlannerPlugin"),
  tf_buffer_(get_clock()),
  tf_listener_(tf_buffer_)
{
  map_frame_ = declare_parameter<std::string>("map_frame");

  planner_ = plugin_loader_.createSharedInstance("mission_planner::lanelet2::DefaultPlanner");
  planner_->initialize(this);

  odometry_ = nullptr;
  sub_odometry_ = create_subscription<Odometry>(
    "/localization/kinematic_state", rclcpp::QoS(1),
    std::bind(&MissionPlanner::on_odometry, this, std::placeholders::_1));
  auto qos_transient_local = rclcpp::QoS{1}.transient_local();
  vector_map_subscriber_ = create_subscription<HADMapBin>(
    "~/input/vector_map", qos_transient_local,
    std::bind(&MissionPlanner::onMap, this, std::placeholders::_1));

  const auto durable_qos = rclcpp::QoS(1).transient_local();
  pub_marker_ = create_publisher<MarkerArray>("debug/route_marker", durable_qos);

  const auto adaptor = component_interface_utils::NodeAdaptor(this);
  adaptor.init_pub(pub_state_);
  adaptor.init_pub(pub_route_);
  adaptor.init_srv(srv_clear_route_, this, &MissionPlanner::on_clear_route);
  adaptor.init_srv(srv_set_route_, this, &MissionPlanner::on_set_route);
  adaptor.init_srv(srv_set_route_points_, this, &MissionPlanner::on_set_route_points);
  adaptor.init_sub(sub_modified_goal_, this, &MissionPlanner::on_modified_goal);

  change_state(RouteState::Message::UNSET);
}

void MissionPlanner::on_odometry(const Odometry::ConstSharedPtr msg)
{
  odometry_ = msg;

  // NOTE: Do not check in the changing state as goal may change.
  if (state_.state == RouteState::Message::SET) {
    PoseStamped pose;
    pose.header = odometry_->header;
    pose.pose = odometry_->pose.pose;
    if (arrival_checker_.is_arrived(pose)) {
      change_state(RouteState::Message::ARRIVED);
    }
  }
}

PoseStamped MissionPlanner::transform_pose(const PoseStamped & input)
{
  PoseStamped output;
  geometry_msgs::msg::TransformStamped transform;
  try {
    transform = tf_buffer_.lookupTransform(map_frame_, input.header.frame_id, tf2::TimePointZero);
    tf2::doTransform(input, output, transform);
    return output;
  } catch (tf2::TransformException & error) {
    throw component_interface_utils::TransformError(error.what());
  }
}

void MissionPlanner::change_route()
{
  arrival_checker_.set_goal();
  // TODO(Takagi, Isamu): publish an empty route here
}

void MissionPlanner::change_route(const LaneletRoute & route)
{
  PoseWithUuidStamped goal;
  goal.header = route.header;
  goal.pose = route.goal_pose;
  goal.uuid = route.uuid;

  arrival_checker_.set_goal(goal);
  pub_route_->publish(route);
  pub_marker_->publish(planner_->visualize(route));
}

void MissionPlanner::change_state(RouteState::Message::_state_type state)
{
  state_.stamp = now();
  state_.state = state;
  pub_state_->publish(state_);
}

// NOTE: The route services should be mutually exclusive by callback group.
void MissionPlanner::on_clear_route(
  const ClearRoute::Service::Request::SharedPtr, const ClearRoute::Service::Response::SharedPtr res)
{
  change_route();
  change_state(RouteState::Message::UNSET);
  res->status.success = true;
}

// NOTE: The route services should be mutually exclusive by callback group.
void MissionPlanner::on_set_route(
  const SetRoute::Service::Request::SharedPtr req, const SetRoute::Service::Response::SharedPtr res)
{
  using ResponseCode = autoware_adapi_v1_msgs::srv::SetRoute::Response;

  if (state_.state != RouteState::Message::UNSET) {
    throw component_interface_utils::ServiceException(
      ResponseCode::ERROR_ROUTE_EXISTS, "The route is already set.");
  }
  if (!odometry_) {
    throw component_interface_utils::ServiceException(
      ResponseCode::ERROR_PLANNER_UNREADY, "The vehicle pose is not received.");
  }

  // Use temporary pose stamped for transform.
  PoseStamped pose;
  pose.header = req->header;
  pose.pose = req->goal;

  // Convert route.
  LaneletRoute route;
  route.start_pose = odometry_->pose.pose;
  route.goal_pose = transform_pose(pose).pose;
  for (const auto & segment : req->segments) {
    route.segments.push_back(convert(segment));
  }
  route.header.stamp = req->header.stamp;
  route.header.frame_id = map_frame_;
  route.uuid.uuid = generate_random_id();

  // Update route.
  change_route(route);
  change_state(RouteState::Message::SET);
  res->status.success = true;
}

// NOTE: The route services should be mutually exclusive by callback group.
void MissionPlanner::on_set_route_points(
  const SetRoutePoints::Service::Request::SharedPtr req,
  const SetRoutePoints::Service::Response::SharedPtr res)
{
  using ResponseCode = autoware_adapi_v1_msgs::srv::SetRoutePoints::Response;

  if (state_.state != RouteState::Message::UNSET) {
    throw component_interface_utils::ServiceException(
      ResponseCode::ERROR_ROUTE_EXISTS, "The route is already set.");
  }
  if (!planner_->ready()) {
    throw component_interface_utils::ServiceException(
      ResponseCode::ERROR_PLANNER_UNREADY, "The planner is not ready.");
  }
  if (!odometry_) {
    throw component_interface_utils::ServiceException(
      ResponseCode::ERROR_PLANNER_UNREADY, "The vehicle pose is not received.");
  }

  // Use temporary pose stamped for transform.
  PoseStamped pose;
  pose.header = req->header;

  // Convert route points.
  PlannerPlugin::RoutePoints points;
  points.push_back(odometry_->pose.pose);
  for (const auto & waypoint : req->waypoints) {
    pose.pose = waypoint;
    points.push_back(transform_pose(pose).pose);
  }
  pose.pose = req->goal;
  points.push_back(transform_pose(pose).pose);

  // Plan route.
  LaneletRoute route = planner_->plan(points);
  if (route.segments.empty()) {
    throw component_interface_utils::ServiceException(
      ResponseCode::ERROR_PLANNER_FAILED, "The planned route is empty.");
  }
  route.header.stamp = req->header.stamp;
  route.header.frame_id = map_frame_;
  route.uuid.uuid = generate_random_id();

  // Update route.
  change_route(route);
  change_state(RouteState::Message::SET);
  res->status.success = true;
}

bool MissionPlanner::checkRerouteSafety(
  const LaneletRoute & original_route, const LaneletRoute & target_route)
{
  if (original_route.segments.empty() || target_route.segments.empty() || !map_ptr_ || !odometry_) {
    return false;
  }

  // find the index of the original route that has same idx with the front segment of the new route
  const auto target_front_primitives = target_route.segments.front().primitives;

  auto hasSamePrimitives = [](
                             const std::vector<LaneletPrimitive> & original_primitives,
                             const std::vector<LaneletPrimitive> & target_primitives) {
    if (original_primitives.size() != target_primitives.size()) {
      return false;
    }

    bool is_same = false;
    for (const auto & primitive : original_primitives) {
      const auto has_same = [&](const auto & p) { return p.id == primitive.id; };
      is_same = std::find_if(target_primitives.begin(), target_primitives.end(), has_same) !=
                target_primitives.end();
    }
    return is_same;
  };

  // find idx that matches the target primitives
  size_t start_idx = original_route.segments.size();
  for (size_t i = 0; i < original_route.segments.size(); ++i) {
    const auto & original_primitives = original_route.segments.at(i).primitives;
    if (hasSamePrimitives(original_primitives, target_front_primitives)) {
      start_idx = i;
      break;
    }
  }

  // find last idx that matches the target primitives
  size_t end_idx = start_idx;
  for (size_t i = 1; i < target_route.segments.size(); ++i) {
    const size_t original_route_idx = start_idx + i;
    if (original_route_idx > original_route.segments.size() - 1) {
      break;
    }

    const auto & original_primitives = original_route.segments.at(original_route_idx).primitives;
    const auto & target_primitives = target_route.segments.at(i).primitives;
    if (!hasSamePrimitives(original_primitives, target_primitives)) {
      end_idx = original_route_idx + 1;
      break;
    }
  }

  // create map
  auto lanelet_map_ptr_ = std::make_shared<lanelet::LaneletMap>();
  lanelet::utils::conversion::fromBinMsg(*map_ptr_, lanelet_map_ptr_);

  // compute distance from the current pose to the end of the current lanelet
  const auto current_pose = target_route.start_pose;
  const auto primitives = original_route.segments.at(start_idx).primitives;
  lanelet::ConstLanelets start_lanelets;
  for (const auto & primitive : primitives) {
    const auto lanelet = lanelet_map_ptr_->laneletLayer.get(primitive.id);
    start_lanelets.push_back(lanelet);
  }

  // get closest lanelet in start lanelets
  lanelet::ConstLanelet closest_lanelet;
  if (!lanelet::utils::query::getClosestLanelet(start_lanelets, current_pose, &closest_lanelet)) {
    return false;
  }

  const auto & centerline_2d = lanelet::utils::to2D(closest_lanelet.centerline());
  const auto lanelet_point = lanelet::utils::conversion::toLaneletPoint(current_pose.position);
  const auto arc_coordinates = lanelet::geometry::toArcCoordinates(
    centerline_2d, lanelet::utils::to2D(lanelet_point).basicPoint());
  const double dist_to_current_pose = arc_coordinates.length;
  const double lanelet_length = lanelet::utils::getLaneletLength2d(closest_lanelet);
  double accumulated_length = lanelet_length - dist_to_current_pose;

  // compute distance from the start_idx+1 to end_idx
  for (size_t i = start_idx + 1; i < end_idx; ++i) {
    const auto primitives = original_route.segments.at(i).primitives;
    if (primitives.empty()) {
      break;
    }

    std::vector<double> lanelets_length(primitives.size());
    for (const auto & primitive : primitives) {
      const auto & lanelet = lanelet_map_ptr_->laneletLayer.get(primitive.id);
      lanelets_length.push_back(lanelet::utils::getLaneletLength2d(lanelet));
    }
    accumulated_length += *std::min_element(lanelets_length.begin(), lanelets_length.end());
  }

  // check safety
  const auto current_velocity = odometry_->twist.twist.linear.x;
  if (accumulated_length > current_velocity * 10.0) {
    return true;
  }

  return false;
}

void MissionPlanner::onMap(const HADMapBin::ConstSharedPtr msg)
{
  map_ptr_ = msg;
}

// NOTE: The route interface should be mutually exclusive by callback group.
void MissionPlanner::on_modified_goal(const ModifiedGoal::Message::ConstSharedPtr msg)
{
  // TODO(Yutaka Shimizu): reroute if the goal is outside the lane.
  arrival_checker_.modify_goal(*msg);
}

}  // namespace mission_planner

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(mission_planner::MissionPlanner)
