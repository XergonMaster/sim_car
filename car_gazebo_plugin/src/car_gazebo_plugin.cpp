#include "car_gazebo_plugin.hpp"

#include <gazebo/physics/physics.hh>
#include <gazebo_ros/node.hpp>
#include <iostream>

namespace car_gazebo_plugin {

CarGazeboPlugin::CarGazeboPlugin()
    : robot_namespace_{""},
      last_sim_time_{0},
      last_update_time_{0},
      update_period_ms_{8} {}

void CarGazeboPlugin::Load(gazebo::physics::ModelPtr model,
                           sdf::ElementPtr sdf) {
  // Get model and world references
  model_ = model;
  world_ = model_->GetWorld();
  auto physicsEngine = world_->Physics();
  physicsEngine->SetParam("friction_model", std::string{"cone_model"});

  if (sdf->HasElement("robotNamespace")) {
    robot_namespace_ =  sdf->GetElement("robotNamespace")->Get<std::string>() + "/";
  }

  // Set up ROS node and subscribers and publishers
  ros_node_ = gazebo_ros::Node::Get(sdf);
  RCLCPP_INFO(ros_node_->get_logger(), "Loading Car Gazebo Plugin");

  rclcpp::QoS qos(10);
  qos.reliability(RMW_QOS_POLICY_RELIABILITY_RELIABLE);
  qos.durability(RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL);

  joint_state_pub_ = ros_node_->create_publisher<JointState>("/joint_states", qos);

  // Find joints
  auto allJoints = model_->GetJoints();
  for (auto const& j : allJoints) {
    if (j->GetType() == gazebo::physics::Joint::FIXED_JOINT) {
      continue;
    }

    auto pid = gazebo::common::PID{};
    pid.SetPGain(200.0);
    pid.SetIGain(0.0);
    pid.SetDGain(0.0);

    auto const& name = j->GetName();
    joints_[name] = std::make_pair(j, pid);
    joint_targets_[name] = 0.0;
  }

  RCLCPP_DEBUG(ros_node_->get_logger(), "Got joints:");
  for (auto const& j : joints_) {
    RCLCPP_DEBUG(ros_node_->get_logger(), j.first.c_str());
  }

  // fake_car code here
  {
    RCLCPP_DEBUG(ros_node_->get_logger(), "Connected to model %s",
                 model_->GetName().c_str());

    jc = model_->GetJointController();

    // front left str
    fl_pid = gazebo::common::PID(1, 0, 0);
    fl_str_joint = get_joint("front_left_wheel_steer_joint");

    jc->SetPositionPID(fl_str_joint->GetScopedName(), fl_pid);

    // front left shock
    fl_shock_pid = gazebo::common::PID(shock_p, 0, shock_d);
    fl_shock_joint = get_joint("front_left_shock_joint");
    jc->SetPositionPID(fl_shock_joint->GetScopedName(), fl_shock_pid);
    jc->SetPositionTarget(fl_shock_joint->GetScopedName(), 0.0);

    // front right shock
    fr_shock_pid = gazebo::common::PID(shock_p, 0, shock_d);
    fr_shock_joint = get_joint("front_right_shock_joint");
    jc->SetPositionPID(fr_shock_joint->GetScopedName(), fr_shock_pid);
    jc->SetPositionTarget(fr_shock_joint->GetScopedName(), 0.0);

    // back left shock
    bl_shock_pid = gazebo::common::PID(shock_p, 0, shock_d);
    bl_shock_joint = get_joint("back_left_shock_joint");
    jc->SetPositionPID(bl_shock_joint->GetScopedName(), bl_shock_pid);
    jc->SetPositionTarget(bl_shock_joint->GetScopedName(), 0.0);

    // back right shock
    br_shock_pid = gazebo::common::PID(shock_p, 0, shock_d);
    br_shock_joint = get_joint("back_right_shock_joint");
    jc->SetPositionPID(br_shock_joint->GetScopedName(), br_shock_pid);
    jc->SetPositionTarget(br_shock_joint->GetScopedName(), 0.0);

    // front right str
    fr_pid = gazebo::common::PID(1, 0, 0);
    fr_str_joint = get_joint("front_right_wheel_steer_joint");
    jc->SetPositionPID(fr_str_joint->GetScopedName(), fr_pid);

    fl_axle_joint = get_joint("front_left_wheel_joint");
    fr_axle_joint = get_joint("front_right_wheel_joint");

    // back left speed
    bl_pid = gazebo::common::PID(0.1, 0.01, 0.0);
    bl_axle_joint = get_joint("back_left_wheel_joint");
    jc->SetVelocityPID(bl_axle_joint->GetScopedName(), bl_pid);

    br_pid = gazebo::common::PID(0.1, 0.01, 0.0);
    br_axle_joint = get_joint("back_right_wheel_joint");
    jc->SetVelocityPID(br_axle_joint->GetScopedName(), br_pid);

    // publish
    odo_fl_pub = ros_node_->create_publisher<std_msgs::msg::Int32>("/" + model_->GetName() + "/odo_fl", 10);
    odo_fr_pub = ros_node_->create_publisher<std_msgs::msg::Int32>("/" + model_->GetName() + "/odo_fr", 10);
    ackermann_pub =
        ros_node_->create_publisher<ackermann_msgs::msg::AckermannDriveStamped>(
            "/" + model_->GetName() + "/cmd_ackermann", 10);

    // subscribe
    joy_sub = ros_node_->create_subscription<sensor_msgs::msg::Joy>(
        "/joy", 2,
        std::bind(&CarGazeboPlugin::joy_callback, this, std::placeholders::_1));

    ackermann_sub =
        ros_node_
            ->create_subscription<ackermann_msgs::msg::AckermannDriveStamped>(
                "/" + model_->GetName() + "/cmd_ackermann", 2,
                std::bind(&CarGazeboPlugin::ackermann_callback, this,
                          std::placeholders::_1));
  }

  // Hook into simulation update loop
  update_connection_ = gazebo::event::Events::ConnectWorldUpdateBegin(
      std::bind(&CarGazeboPlugin::Update, this));
}

void CarGazeboPlugin::Update() {
  auto cur_time = world_->SimTime();
  if (last_sim_time_ == 0) {
    last_sim_time_ = cur_time;
    last_update_time_ = cur_time;
    return;
  }


  auto dt = (cur_time - last_sim_time_).Double();

  // Publish joint states
  auto update_dt = (cur_time - last_update_time_).Double();
  if (update_dt * 1000 >= update_period_ms_) {
    publish_state();
    auto msg = JointState{};
    msg.header.stamp = ros_node_->now();

    for (auto& j : joints_) {
      auto const& name = j.first;
      auto& joint = j.second.first;
      auto position = joint->Position();
      msg.name.push_back(name);
      msg.position.push_back(position);
    }
    joint_state_pub_->publish(msg);
    last_update_time_ = cur_time;
  }

  last_sim_time_ = cur_time;
}

GZ_REGISTER_MODEL_PLUGIN(CarGazeboPlugin)

}  // namespace car_gazebo_plugin