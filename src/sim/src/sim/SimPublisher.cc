#include "sim/SimPublisher.h"

namespace clear {

SimPublisher::SimPublisher(mj::Simulate *sim)
    : Node("SimPublisher"), sim_(sim), name_prefix("simulation/") {
  model_param_name = name_prefix + "model_file";
  this->declare_parameter(model_param_name, "");

  reset_service_ = this->create_service<trans::srv::SimulationReset>(
      name_prefix + "sim_reset",
      std::bind(&SimPublisher::reset_callback, this, std::placeholders::_1,
                std::placeholders::_2));

  auto qos = rclcpp::QoS(rclcpp::KeepLast(1), rmw_qos_profile_sensor_data);
  imu_publisher_ = this->create_publisher<sensor_msgs::msg::Imu>(
      name_prefix + "imu_data", qos);
  joint_state_publisher_ = this->create_publisher<sensor_msgs::msg::JointState>(
      name_prefix + "joint_states", qos);
  odom_publisher_ = this->create_publisher<nav_msgs::msg::Odometry>(
      name_prefix + "odom", qos);
  touch_publisher_ = this->create_publisher<trans::msg::TouchSensor>(
      name_prefix + "touch_sensor", qos);

  timers_.emplace_back(this->create_wall_timer(
      2.5ms, std::bind(&SimPublisher::imu_callback, this)));
  timers_.emplace_back(this->create_wall_timer(
      1ms, std::bind(&SimPublisher::joint_callback, this)));
  timers_.emplace_back(this->create_wall_timer(
      20ms, std::bind(&SimPublisher::odom_callback, this)));
  timers_.emplace_back(this->create_wall_timer(
      2ms, std::bind(&SimPublisher::touch_callback, this)));
  timers_.emplace_back(this->create_wall_timer(
      100ms, std::bind(&SimPublisher::drop_old_message, this)));
  /* timers_.emplace_back(this->create_wall_timer(
      4s, std::bind(&SimPublisher::throw_box, this)));
 */
  actuator_cmd_subscription_ =
      this->create_subscription<trans::msg::ActuatorCmds>(
          name_prefix + "actuators_cmds", qos,
          std::bind(&SimPublisher::actuator_cmd_callback, this,
                    std::placeholders::_1));

  actuator_cmds_buffer_ = std::make_shared<ActuatorCmdsBuffer>();

  RCLCPP_INFO(this->get_logger(), "Start SimPublisher ...");

  std::string model_file = this->get_parameter(model_param_name)
                               .get_parameter_value()
                               .get<std::string>();
  mju::strcpy_arr(sim_->filename, model_file.c_str());
  sim_->uiloadrequest.fetch_add(1);
}

SimPublisher::~SimPublisher() {
  RCLCPP_INFO(this->get_logger(), "close SimPublisher node ...");
}

void SimPublisher::reset_callback(
    const std::shared_ptr<trans::srv::SimulationReset::Request> request,
    std::shared_ptr<trans::srv::SimulationReset::Response> response) {
  while (sim_->d_ == nullptr && rclcpp::ok()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (sim_->d_ != nullptr) {
    if (request->header.frame_id != std::string(&sim_->m_->names[0])) {
      RCLCPP_ERROR(this->get_logger(), "reset request is not for %s",
                   &sim_->m_->names[0]);
      response->is_success = false;
    } else {
      {
        const std::unique_lock<std::recursive_mutex> lock(sim_->mtx);
        mj_resetData(sim_->m_, sim_->d_);
        sim_->d_->qpos[0] = request->base_pose.position.x;
        sim_->d_->qpos[1] = request->base_pose.position.y;
        sim_->d_->qpos[2] = request->base_pose.position.z;
        sim_->d_->qpos[3] = request->base_pose.orientation.w;
        sim_->d_->qpos[4] = request->base_pose.orientation.x;
        sim_->d_->qpos[5] = request->base_pose.orientation.y;
        sim_->d_->qpos[6] = request->base_pose.orientation.z;

        for (size_t i = 0; i < request->joint_state.position.size(); i++) {
          int joint_id = mj_name2id(sim_->m_, mjOBJ_JOINT,
                                    request->joint_state.name[i].c_str());
          if (joint_id > -1) {
            sim_->d_->qpos[sim_->m_->jnt_qposadr[joint_id]] =
                request->joint_state.position[i];
          } else {
            RCLCPP_WARN(this->get_logger(), "[Reset Request] joint %s does not exist",
                        request->joint_state.name[i].c_str());
          }
        }
        for (size_t k = 0; k < actuator_cmds_buffer_->actuators_name.size();
             k++) {
          actuator_cmds_buffer_->kp[k] = 0;
          actuator_cmds_buffer_->pos[k] = 0;
          actuator_cmds_buffer_->kd[k] = 0;
          actuator_cmds_buffer_->vel[k] = 0;
          actuator_cmds_buffer_->torque[k] = 0.0;
        }
      }
      response->is_success = true;
      RCLCPP_INFO(this->get_logger(), "reset robot state...");
    }
  } else {
    response->is_success = false;
  }
}

void SimPublisher::imu_callback() {
  if (sim_->d_ != nullptr) {
    auto message = sensor_msgs::msg::Imu();
    message.header.frame_id = &sim_->m_->names[0];
    message.header.stamp = rclcpp::Clock().now();
    {
      const std::unique_lock<std::recursive_mutex> lock(sim_->mtx);
      bool acc_flag = true;
      bool gyro_flag = true;
      bool quat_flag = true;
      for (int i = 0; i < sim_->m_->nsensor; i++) {
        if (sim_->m_->sensor_type[i] == mjtSensor::mjSENS_ACCELEROMETER) {
          message.linear_acceleration.x =
              sim_->d_->sensordata[sim_->m_->sensor_adr[i]];
          message.linear_acceleration.y =
              sim_->d_->sensordata[sim_->m_->sensor_adr[i] + 1];
          message.linear_acceleration.z =
              sim_->d_->sensordata[sim_->m_->sensor_adr[i] + 2];
          acc_flag = false;
        } else if (sim_->m_->sensor_type[i] == mjtSensor::mjSENS_FRAMEQUAT) {
          message.orientation.w = sim_->d_->sensordata[sim_->m_->sensor_adr[i]];
          message.orientation.x =
              sim_->d_->sensordata[sim_->m_->sensor_adr[i] + 1];
          message.orientation.y =
              sim_->d_->sensordata[sim_->m_->sensor_adr[i] + 2];
          message.orientation.z =
              sim_->d_->sensordata[sim_->m_->sensor_adr[i] + 3];
          quat_flag = false;
        } else if (sim_->m_->sensor_type[i] == mjtSensor::mjSENS_GYRO) {
          message.angular_velocity.x =
              sim_->d_->sensordata[sim_->m_->sensor_adr[i]];
          message.angular_velocity.y =
              sim_->d_->sensordata[sim_->m_->sensor_adr[i] + 1];
          message.angular_velocity.z =
              sim_->d_->sensordata[sim_->m_->sensor_adr[i] + 2];
          gyro_flag = false;
        }
      }
      if (acc_flag) {
        RCLCPP_WARN(this->get_logger(), "Required acc sensor does not exist");
      }
      if (quat_flag) {
        RCLCPP_WARN(this->get_logger(), "Required quat sensor does not exist");
      }
      if (gyro_flag) {
        RCLCPP_WARN(this->get_logger(), "Required gyro sensor does not exist");
      }
    }
    imu_publisher_->publish(message);
  }
}

void SimPublisher::touch_callback() {
  if (sim_->d_ != nullptr) {
    auto message = trans::msg::TouchSensor();
    message.header.frame_id = &sim_->m_->names[0];
    message.header.stamp = rclcpp::Clock().now();
    {
      const std::unique_lock<std::recursive_mutex> lock(sim_->mtx);
      for (int i = 0; i < sim_->m_->nsensor; i++) {
        if (sim_->m_->sensor_type[i] == mjtSensor::mjSENS_TOUCH) {
          message.names.emplace_back(
              mj_id2name(sim_->m_, mjtObj::mjOBJ_SENSOR, i));
          message.value.emplace_back(
              sim_->d_->sensordata[sim_->m_->sensor_adr[i]]);
        }
      }
    }
    touch_publisher_->publish(message);
  }
}

void SimPublisher::odom_callback() {
  if (sim_->d_ != nullptr) {
    auto message = nav_msgs::msg::Odometry();
    {
      const std::unique_lock<std::recursive_mutex> lock(sim_->mtx);
      message.header.frame_id = &sim_->m_->names[0];
      message.header.stamp = rclcpp::Clock().now();
      message.pose.pose.position.x = sim_->d_->qpos[0];
      message.pose.pose.position.y = sim_->d_->qpos[1];
      message.pose.pose.position.z = sim_->d_->qpos[2];
      message.pose.pose.orientation.w = sim_->d_->qpos[3];
      message.pose.pose.orientation.x = sim_->d_->qpos[4];
      message.pose.pose.orientation.y = sim_->d_->qpos[5];
      message.pose.pose.orientation.z = sim_->d_->qpos[6];
      message.twist.twist.linear.x = sim_->d_->qvel[0];
      message.twist.twist.linear.y = sim_->d_->qvel[1];
      message.twist.twist.linear.z = sim_->d_->qvel[2];
      message.twist.twist.angular.x = sim_->d_->qvel[3];
      message.twist.twist.angular.y = sim_->d_->qvel[4];
      message.twist.twist.angular.z = sim_->d_->qvel[5];
    }
    odom_publisher_->publish(message);
  }
}

void SimPublisher::joint_callback() {
  if (sim_->d_ != nullptr) {
    sensor_msgs::msg::JointState jointState;
    jointState.header.frame_id = &sim_->m_->names[0];
    jointState.header.stamp = rclcpp::Clock().now();
    {
      const std::unique_lock<std::recursive_mutex> lock(sim_->mtx);
      for (int i = 0; i < sim_->m_->njnt; i++) {
        if (sim_->m_->jnt_type[i] != mjtJoint::mjJNT_FREE) {
          std::string jnt_name(mj_id2name(sim_->m_, mjtObj::mjOBJ_JOINT, i));
          jointState.name.emplace_back(jnt_name);
          jointState.position.push_back(
              sim_->d_->qpos[sim_->m_->jnt_qposadr[i]]);
          jointState.velocity.push_back(
              sim_->d_->qvel[sim_->m_->jnt_dofadr[i]]);
          jointState.effort.push_back(
              sim_->d_->qfrc_actuator[sim_->m_->jnt_dofadr[i]]);
        }
      }
    }
    joint_state_publisher_->publish(jointState);
  }
}

void SimPublisher::actuator_cmd_callback(
    const trans::msg::ActuatorCmds::SharedPtr msg) const {
  if (sim_->d_ != nullptr) {
    actuator_cmds_buffer_->time = rclcpp::Time(msg->header.stamp).seconds();
    actuator_cmds_buffer_->actuators_name.resize(msg->names.size());
    actuator_cmds_buffer_->kp.resize(msg->gain_p.size());
    actuator_cmds_buffer_->pos.resize(msg->pos_des.size());
    actuator_cmds_buffer_->kd.resize(msg->gaid_d.size());
    actuator_cmds_buffer_->vel.resize(msg->vel_des.size());
    actuator_cmds_buffer_->torque.resize(msg->feedforward_torque.size());
    for (size_t k = 0; k < msg->names.size(); k++) {
      actuator_cmds_buffer_->actuators_name[k] = msg->names[k];
      actuator_cmds_buffer_->kp[k] = msg->gain_p[k];
      actuator_cmds_buffer_->pos[k] = msg->pos_des[k];
      actuator_cmds_buffer_->kd[k] = msg->gaid_d[k];
      actuator_cmds_buffer_->vel[k] = msg->vel_des[k];
      actuator_cmds_buffer_->torque[k] = msg->feedforward_torque[k];
    }
    // RCLCPP_INFO(this->get_logger(), "subscribe actuator cmds %f",
    // actuator_cmds_buffer_->time);
  }
}

void SimPublisher::drop_old_message() {
  if (abs(actuator_cmds_buffer_->time - this->now().seconds()) > 0.2) {
    for (size_t k = 0; k < actuator_cmds_buffer_->actuators_name.size(); k++) {
      actuator_cmds_buffer_->kp[k] = 0.0;
      actuator_cmds_buffer_->pos[k] = 0.0;
      actuator_cmds_buffer_->kd[k] = 1.0;
      actuator_cmds_buffer_->vel[k] = 0.0;
      actuator_cmds_buffer_->torque[k] = 0.0;
    }
  }
}

void SimPublisher::throw_box() {
  const std::unique_lock<std::recursive_mutex> lock(sim_->mtx);

  int nq = sim_->m_->nq - 1;
  int nv = sim_->m_->nv - 1;
  int nq_shift = 0;
  int nv_shift = 0;

  if (sim_->d_->time < 5.0) {
    return;
  }
  for (int i = 0; i < 4; i++) {
    std::vector<mjtNum> pos;
    std::vector<mjtNum> vel;

    switch (i) {
    case 0:
      pos = {0.45, 0, 0.5};
      vel = {0, 0, -1.5};
      break;

    case 1:
      pos = {0.15, -0.5, 0.2};
      vel = {0, 2.5, 0};
      break;

    case 2:
      pos = {-0.15, 0.5, 0.2};
      vel = {0, -2.5, 0};
      break;

    case 3:
      pos = {0.5, 0.5, 0.5};
      vel = {-2.0, -2.0, -2.0};
      break;

    default:
      break;
    }
    sim_->d_->qpos[nq - nq_shift] = 0;
    sim_->d_->qpos[nq - 1 - nq_shift] = 0;
    sim_->d_->qpos[nq - 2 - nq_shift] = 0;
    sim_->d_->qpos[nq - 3 - nq_shift] = 1;
    sim_->d_->qpos[nq - 4 - nq_shift] = sim_->d_->qpos[2] + pos[2];
    sim_->d_->qpos[nq - 5 - nq_shift] = sim_->d_->qpos[1] + pos[1];
    sim_->d_->qpos[nq - 6 - nq_shift] = sim_->d_->qpos[0] + pos[0];

    sim_->d_->qvel[nv - nv_shift] = 0;
    sim_->d_->qvel[nv - 1 - nv_shift] = 0;
    sim_->d_->qvel[nv - 2 - nv_shift] = 0;
    sim_->d_->qvel[nv - 3 - nv_shift] = sim_->d_->qvel[2] + vel[2];
    sim_->d_->qvel[nv - 4 - nv_shift] = sim_->d_->qvel[1] + vel[1];
    sim_->d_->qvel[nv - 5 - nv_shift] = sim_->d_->qvel[0] + vel[0];

    nq_shift += 7;
    nv_shift += 6;
  }
}

std::shared_ptr<ActuatorCmdsBuffer> SimPublisher::get_cmds_buffer() {
  return actuator_cmds_buffer_;
}

} // namespace clear
