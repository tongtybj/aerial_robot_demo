/*
 * Copyright (c) 2016, JSK Robotics Laboratory, The University of Tokyo
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <mbzirc2020_task1_common/mbzirc_gazebo_treasure_plugin.h>
#include <string>

namespace gazebo
{

  GazeboTreasure::GazeboTreasure()
  {
    magnet_on_.data = false;
    pirate_linear_acc_.Set(0.0, 0.0, 9.8);
  }

  GazeboTreasure::~GazeboTreasure()
  {
#if GAZEBO_MAJOR_VERSION >= 8
    update_connection_.reset();
#else
    event::Events::DisconnectWorldUpdateBegin(update_connection_);
#endif

    node_handle_->shutdown();
    delete node_handle_;
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Load the controller
  void GazeboTreasure::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
  {
    model_ = _model;
    world_ = _model->GetWorld();
    link_ = _model->GetLink();
    link_name_ = link_->GetName();
    namespace_.clear();
    model_->SetGravityMode(false);

    static_object_ = false;
#if GAZEBO_MAJOR_VERSION >= 8
    last_time_ = world_->SimTime();
#else
    last_time_ = world_->GetSimTime();
#endif
    terminated_ = false;

    treasure_state_ = TREASURE_CRUISE;

    // load parameters from sdf
    if (_sdf->HasElement("robotNamespace")) namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>();

    if (_sdf->HasElement("bodyName") && _sdf->GetElement("bodyName")->GetValue())
      {
        link_name_ = _sdf->GetElement("bodyName")->Get<std::string>();
        link_ = _model->GetLink(link_name_);
      }

    if (_sdf->HasElement("staticObject") && _sdf->GetElement("staticObject")->GetValue())
      {
        static_object_ = _sdf->GetElement("staticObject")->Get<std::string>() == "true"?true:false;
      }

    if (!link)
      {
        ROS_FATAL("gazebo_ros_baro plugin error: bodyName: %s does not exist\n", link_name_.c_str());
        return;
      }

    if (_sdf->HasElement("guardName") && _sdf->GetElement("guardName")->GetValue())
      guard_name_ = _sdf->GetElement("guardName")->Get<std::string>();
    else
      guard_name_ = "hawk";

    if (_sdf->HasElement("pirateName") && _sdf->GetElement("pirateName")->GetValue())
      pirate_name_ = _sdf->GetElement("pirateName")->Get<std::string>();
    else
      pirate_name_ = "hydrus";

    if (_sdf->HasElement("guardUavTreasureOffsetZ") && _sdf->GetElement("guardUavTreasureOffsetZ")->GetValue())
      guard_uav_treasure_offset_z_ = _sdf->GetElement("guardUavTreasureOffsetZ")->Get<double>();
    else
      guard_uav_treasure_offset_z_ = 0.27;

    if (_sdf->HasElement("pirateUavTreasureOffsetZ") && _sdf->GetElement("pirateUavTreasureOffsetZ")->GetValue())
      pirate_uav_treasure_offset_z_ = _sdf->GetElement("pirateUavTreasureOffsetZ")->Get<double>();
    else
      pirate_uav_treasure_offset_z_ = 0.03;

    if (_sdf->HasElement("grabThreshold") && _sdf->GetElement("grabThreshold")->GetValue())
      grab_thre_ = _sdf->GetElement("grabThreshold")->Get<double>();
    else
      grab_thre_ = 0.3;

    std::string treasure_point_topic_name;
    if (_sdf->HasElement("treasurePositionTopicName") && _sdf->GetElement("treasurePositionTopicName")->GetValue())
      treasure_point_topic_name = _sdf->GetElement("treasurePositionTopicName")->Get<std::string>();
    else
      treasure_point_topic_name = "/treasure/point_from_gazebo";

    std::string treasure_marker_topic_name;
    if (_sdf->HasElement("markerTopicName") && _sdf->GetElement("markerTopicName")->GetValue())
      treasure_marker_topic_name = _sdf->GetElement("markerTopicName")->Get<std::string>();
    else
      treasure_marker_topic_name = "/treasure/marker";

    std::string treasure_capture_flag_topic_name;
    if (_sdf->HasElement("captureFlagTopicName") && _sdf->GetElement("captureFlagTopicName")->GetValue())
      treasure_capture_flag_topic_name = _sdf->GetElement("captureFlagTopicName")->Get<std::string>();
    else
      treasure_capture_flag_topic_name = "/treasure/capture_flag";

    std::string pirate_imu_topic_name;
    if (_sdf->HasElement("pirateImuTopicName") && _sdf->GetElement("pirateImuTopicName")->GetValue())
      pirate_imu_topic_name = _sdf->GetElement("pirateImuTopicName")->Get<std::string>();
    else
      pirate_imu_topic_name = "/hawk/raw_imu";

    // Make sure the ROS node for Gazebo has already been initialized
    if (!ros::isInitialized())
      {
        ROS_FATAL_STREAM("A ROS node for Gazebo has not been initialized, unable to load plugin. "
                         << "Load the Gazebo system plugin 'libgazebo_ros_api_plugin.so' in the gazebo_ros package)");
        return;
      }

    node_handle_ = new ros::NodeHandle(namespace_);
    pub_score_ = node_handle_->advertise<std_msgs::String>("score", 1, true);  // set latch true
    ros::NodeHandle param_handle(*node_handle_, "controller");

    // subscribe to the gazebo models
    gazebo_model_sub_ = node_handle_->subscribe("/gazebo/model_states",3,&GazeboTreasure::gazeboCallback,this);
    magnet_release_sub_ = node_handle_->subscribe("/mag_on",3,&GazeboTreasure::magnetCallback,this);
    treasure_force_state_sub_ = node_handle_->subscribe("/treasure_force_state_cmd",3,&GazeboTreasure::treasureForceStateCallback,this);
    pirate_imu_sub_ = node_handle_->subscribe(pirate_imu_topic_name,3,&GazeboTreasure::pirateImuCallback,this);

    // publisher
    treasure_point_pub_ = node_handle_->advertise<geometry_msgs::PointStamped>(treasure_point_topic_name, 1, true);
    treasure_marker_pub_ = node_handle_->advertise<visualization_msgs::Marker>(treasure_marker_topic_name, 1, true);
    treasure_capture_flag_pub_ = node_handle_->advertise<std_msgs::Bool>(treasure_capture_flag_topic_name, 1, true);

    update_connection_ = event::Events::ConnectWorldUpdateBegin(
                                                                boost::bind(&GazeboTreasure::Update, this));

  }

  void GazeboTreasure::treasureForceStateCallback(std_msgs::UInt8 msg){
    if (msg.data == TREASURE_RELEASE)
      model_->SetGravityMode(true);
    else if (treasure_state_ == TREASURE_RELEASE) // previous state is release state
      model_->SetGravityMode(false);
    treasure_state_ = msg.data;
  }


  ////////////////////////////////////////////////////////////////////////////////
  // Update the controller
  void GazeboTreasure::Update()
  {
    if ( terminated_ )
      {
        return;
      }
    else
      {
        int pirate_id = -1, guard_id = -1;
        for(int i = 0; i < this->gazebo_models_.name.size(); i++){
          if (gazebo_models_.name.at(i) == pirate_name_)
            pirate_id = i;
          else if (gazebo_models_.name.at(i) == guard_name_)
            guard_id = i;
        }
        if (treasure_state_ == TREASURE_FORCE_INIT){
          if (guard_id >= 0){
            updateTreasureState(guard_id, guard_name_);
            treasure_state_ = TREASURE_CRUISE;
          }
        }
        else if (treasure_state_ == TREASURE_CRUISE){
          if (guard_id >= 0){
            // reset object positon
            updateTreasureState(guard_id, guard_name_);
          }
          if (pirate_id >= 0){
            // judge whether treasure is grabbed
            geometry_msgs::Pose pose_pirate = gazebo_models_.pose.at(pirate_id);
#if GAZEBO_MAJOR_VERSION >= 8
            math::Pose3d pose_object = link_->WorldPose();  //the pose of the object
            if(fabs(pose_pirate.position.x - pose_object.Pos().X())<grab_thre_&&
               fabs(pose_pirate.position.y - pose_object.Pos().Y())<grab_thre_&&
               fabs(pose_pirate.position.z - pose_object.Pos().Z())<grab_thre_ / 2.0){
#else
            math::Pose pose_object = link_->GetWorldPose();  //the pose of the object
            if(fabs(pose_pirate.position.x - pose_object.pos.x)<grab_thre_&&
               fabs(pose_pirate.position.y - pose_object.pos.y)<grab_thre_&&
               fabs(pose_pirate.position.z - pose_object.pos.z)<grab_thre_ / 2.0){
#endif
              treasure_state_ = TREASURE_CAPTURED;
              ROS_ERROR("TREASURE_CAPTURED");
              updateTreasureState(pirate_id, pirate_name_);
            }
          }
        }
        else if (treasure_state_ == TREASURE_CAPTURED){
          if (pirate_id >= 0){
            // reset object positon
            updateTreasureState(pirate_id, pirate_name_);
            std_msgs::Bool capture_flag_msg;
            capture_flag_msg.data = true;
            treasure_capture_flag_pub_.publish(capture_flag_msg);
          }
        }
        else if (treasure_state_ == TREASURE_RELEASE)
          visualizeTreasure();
      }

    if( static_object_ )
      {
        return;
      }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Reset the controller
  void GazeboTreasure::Reset()
  {
    state_stamp_ = ros::Time();
  }

  void GazeboTreasure::updateTreasureState(int owner_id, std::string robot_name){
    geometry_msgs::Pose treausre_pose = gazebo_models_.pose.at(owner_id);
#if GAZEBO_MAJOR_VERSION >= 8
    math::Vector3d offset;
#else
    math::Vector3 offset;
#endif

    if (robot_name == std::string("hawk")){
      if (treausre_pose.position.z < guard_uav_treasure_offset_z_) // drone is in low height
        offset.Set(0.0, 0.0, treausre_pose.position.z);
      else
        offset = pirate_linear_acc_ / 9.8 * guard_uav_treasure_offset_z_;
    }
    else if (robot_name == std::string("hydrus"))
      offset.Set(0.0, 0.0, pirate_uav_treasure_offset_z_);

#if GAZEBO_MAJOR_VERSION >= 8
    model_->SetWorldPose(math::Pose3d(math::Vector3d(treausre_pose.position.x,
                                                     treausre_pose.position.y,
                                                     treausre_pose.position.z)
                                      - offset,
                                      math::Quaterniond(1, 0, 0, 0)));
#else
    model_->SetWorldPose(math::Pose(math::Vector3(treausre_pose.position.x,
                                                  treausre_pose.position.y,
                                                  treausre_pose.position.z)
                                    - offset,
                                    math::Quaternion(1, 0, 0, 0)));
#endif
    visualizeTreasure();
  }

  void GazeboTreasure::visualizeTreasure(){

    geometry_msgs::Pose treausre_pose;
#if GAZEBO_MAJOR_VERSION >= 8
    math::Pose3d pose_object = link_->WorldPose();
    treausre_pose.position.x = pose_object.Pos().X();
    treausre_pose.position.y = pose_object.Pos().Y();
    treausre_pose.position.z = pose_object.Pos().Z();
#else
    math::Pose pose_object = link_->GetWorldPose();
    treausre_pose.position.x = pose_object.pos.x;
    treausre_pose.position.y = pose_object.pos.y;
    treausre_pose.position.z = pose_object.pos.z;
#endif
    // publish the treasure marker in the rviz
    visualization_msgs::Marker treasure_marker;
    treasure_marker.ns = "treasure_marker";
    treasure_marker.id = 0;
    treasure_marker.type = visualization_msgs::Marker::SPHERE;
    treasure_marker.action = visualization_msgs::Marker::ADD;
    double ball_radius = 0.075 * 2;
    treasure_marker.scale.x = ball_radius;
    treasure_marker.scale.y = ball_radius;
    treasure_marker.scale.z = ball_radius;
    treasure_marker.color.a = 1.0; // Don't forget to set the alpha!
    treasure_marker.color.r = 1.0;
    treasure_marker.color.g = 1.0;
    treasure_marker.color.b = 0.0;
    treasure_marker.header.frame_id = "world";
    treasure_marker.header.stamp = ros::Time::now();
    treasure_marker.pose = treausre_pose;
    treasure_marker_pub_.publish(treasure_marker);

    geometry_msgs::PointStamped treasure_point;
    treasure_point.header = treasure_marker.header;
    treasure_point.point.x = treausre_pose.position.x;
    treasure_point.point.y = treausre_pose.position.y;
    treasure_point.point.z = treausre_pose.position.z;
    treasure_point_pub_.publish(treasure_point);
  }

  // Register this plugin with the simulator
  GZ_REGISTER_MODEL_PLUGIN(GazeboTreasure)

}  // namespace gazebo
