#include <mbzirc2020_task1_tasks/ReactiveMotion.h>

ReactiveMotion::ReactiveMotion(ros::NodeHandle nh, ros::NodeHandle nhp){
  nh_ = nh;
  nhp_ = nhp;

  nhp_.param("control_frequency", control_freq_, 100.0);
  nhp_.param("cog_net_offset", cog_net_offset_, 0.15);
  nhp_.param("target_pos_xy_threshold", target_pos_xy_thre_, 3.0);
  nhp_.param("experiment_safety_z_offset", experiment_safety_z_offset_, 0.0);
  nhp_.getParam("motion_cmd_threshold_list", motion_cmd_thre_vec_);

  ransac_line_estimator_ = new RansacLineFitting(nh_, nhp_);
  motion_state_ = STILL;
  // motion_state_ = WAITING;
  task_initial_waiting_pos_flag_ = false;

  control_timer_ = nh_.createTimer(ros::Duration(1.0 / control_freq_), &ReactiveMotion::controlTimerCallback, this);

  cog_odom_sub_ = nh_.subscribe<nav_msgs::Odometry>("/uav/cog/odom", 1, &ReactiveMotion::cogOdomCallback, this, ros::TransportHints().tcpNoDelay());
  motion_state_sub_ = nh_.subscribe<std_msgs::Int8>("/reactive_motion/state", 1, &ReactiveMotion::reactiveMotionStateCallback, this, ros::TransportHints().tcpNoDelay());

  flight_nav_pub_ = nh_.advertise<aerial_robot_msgs::FlightNav>("/uav/nav", 1);
  // nearest_waypoint_pub_ = nh_.advertise<visualization_msgs::Marker>("/reactive_motion/target", 1);
  nearest_waypoint_pub_ = nh_.advertise<geometry_msgs::PointStamped>("/reactive_motion/target", 1);
  uav_cog_point_pub_ = nh_.advertise<geometry_msgs::PointStamped>("/reactive_motion/cog_point", 1);

  sleep(0.1);
}

void ReactiveMotion::controlTimerCallback(const ros::TimerEvent& event){
  if (motion_state_ == STILL)
    return;
  else if (motion_state_ == WAITING){
    if (ransac_line_estimator_->isEstimated()){
      motion_state_ = TRACKING;
      ROS_INFO("[ReactiveMotion] Ransac result is updated, start to track target");
    }
  }
  if (motion_state_ == TRACKING){
    if (!ransac_line_estimator_->isEstimated()){ // target is losing
      motion_state_ = WAITING;
      ROS_INFO("[ReactiveMotion] Ransac result is losing, keep waiting");
      sendControlCmd(cur_pos_);
      return;
    }
    double net_cog_yaw_offset = -M_PI / 4.0;
    if (!ransac_line_estimator_->checkEstimationWithYawAng(euler_ang_[2] + net_cog_yaw_offset)){
      ROS_WARN("[ReactiveMotion] Bad estimation varies robot orientation, follow previous cmd");
      return;
    }
    if (ransac_line_estimator_->isNearTarget(cur_pos_)){ // target is too near
      motion_state_ = STOP_TRACKING;
      stop_tracking_cnt_ = 0;
      ROS_INFO("[ReactiveMotion] Target is very near, track previous waypoint in open-loop way");
      sendControlCmd(target_pos_);
      return;
    }
    if (ransac_line_estimator_->getNearestWaypoint(cur_pos_, target_pos_)){
      if (isTargetPosInSearchRegion())
        sendControlCmd(target_pos_);
      else{
        ROS_WARN("[ReactiveMotion] Estimation is out of initial search region, follow previous cmd");
        return;
      }
    }
    else{
      ROS_ERROR("[ReactiveMotion] Could not get nearest waypoint, change to WAITING mode");
      motion_state_ = WAITING;
    }
  }
  else if (motion_state_ == STOP_TRACKING){
    sendControlCmd(target_pos_);
    ++stop_tracking_cnt_;
    double stop_tracking_period = 3.0;
    if (stop_tracking_cnt_ >= stop_tracking_period * control_freq_){
      motion_state_ = STILL;
      sendControlCmd(cur_pos_);
      ROS_INFO("[ReactiveMotion] After open-loop tracking for %f secs, switch back to STILL mode.", stop_tracking_period);
    }
  }
}

void ReactiveMotion::sendControlCmd(Eigen::Vector3d target_pos){
  geometry_msgs::PointStamped target_pt_msg;
  target_pt_msg.header.stamp = ros::Time::now();
  target_pt_msg.header.frame_id = "world";
  target_pt_msg.point.x = target_pos(0);
  target_pt_msg.point.y = target_pos(1);
  target_pt_msg.point.z = target_pos(2);
  nearest_waypoint_pub_.publish(target_pt_msg);

  Eigen::Vector3d delta_pos = target_pos - cur_pos_;
  for (int i = 0; i < 3; ++i){
    if (delta_pos[i] > motion_cmd_thre_vec_[i])
      delta_pos[i] = motion_cmd_thre_vec_[i];
    else if (delta_pos[i] < -motion_cmd_thre_vec_[i])
      delta_pos[i] = -motion_cmd_thre_vec_[i];
  }
  aerial_robot_msgs::FlightNav nav_msg;
  nav_msg.header.stamp = ros::Time::now();
  nav_msg.header.frame_id = "world";
  nav_msg.control_frame = nav_msg.WORLD_FRAME;
  nav_msg.target = nav_msg.COG;
  nav_msg.pos_xy_nav_mode = nav_msg.POS_MODE;
  nav_msg.target_pos_x = cur_pos_(0) + delta_pos(0);
  nav_msg.target_pos_y = cur_pos_(1) + delta_pos(1);
  nav_msg.pos_z_nav_mode = nav_msg.POS_MODE;
  nav_msg.target_pos_z = cur_pos_(2) + delta_pos(2);
  nav_msg.target_pos_z += cog_net_offset_;
  nav_msg.target_pos_z += experiment_safety_z_offset_;
  // todo: add psi
  // nav_msg.psi_nav_mode = nav_msg.POS_MODE;
  // nav_msg.target_psi = -math.pi / 2.0 # 0.0
  flight_nav_pub_.publish(nav_msg);
}

bool ReactiveMotion::isTargetPosInSearchRegion(){
  for (int i = 0; i < 2; ++i){
    if (fabs(target_pos_(i) - task_initial_waiting_pos_(i)) > target_pos_xy_thre_)
      return false;
  }
  return true;
}

void ReactiveMotion::cogOdomCallback(const nav_msgs::OdometryConstPtr & msg){
  cog_odom_ = *msg;
  cur_pos_ << cog_odom_.pose.pose.position.x,
    cog_odom_.pose.pose.position.y,
    cog_odom_.pose.pose.position.z;
  tf::Quaternion q(cog_odom_.pose.pose.orientation.x,
                   cog_odom_.pose.pose.orientation.y,
                   cog_odom_.pose.pose.orientation.z,
                   cog_odom_.pose.pose.orientation.w);
  tf::Matrix3x3  uav_rot_mat(q);
  uav_rot_mat.getRPY(euler_ang_[0], euler_ang_[1], euler_ang_[2]);
  geometry_msgs::PointStamped cog_pt_msg;
  cog_pt_msg.header = cog_odom_.header;
  cog_pt_msg.header.frame_id = "world";
  cog_pt_msg.point = cog_odom_.pose.pose.position;
  uav_cog_point_pub_.publish(cog_pt_msg);
}

void ReactiveMotion::reactiveMotionStateCallback(const std_msgs::Int8ConstPtr & msg){
  if (msg->data == 0){
    motion_state_ = STILL;
    sendControlCmd(cur_pos_);
    ROS_INFO("[ReactiveMotion] Change motion state: STILL");
  }
  else if (msg->data == 1){
    motion_state_ = WAITING;
    ROS_INFO("[ReactiveMotion] Change motion state: WAITING");
    if (!task_initial_waiting_pos_flag_){
      task_initial_waiting_pos_flag_ = true;
      task_initial_waiting_pos_ = cur_pos_;
      ransac_line_estimator_->startEstimation();
      ROS_WARN("[ReactiveMotion] Start waiting pos recorded, estimation prepared to work.");
    }
  }
  else if (msg->data == 2){
    motion_state_ = TRACKING;
    ROS_INFO("[ReactiveMotion] Change motion state: TRACKING");
  }
  else if (msg->data == 3){
    motion_state_ = STOP_TRACKING;
    ROS_INFO("[ReactiveMotion] Change motion state: STOP_TRACKING");
  }
}