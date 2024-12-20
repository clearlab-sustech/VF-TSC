#include <MotionManager.h>
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <core/misc/Benchmark.h>
#include <pinocchio/Orientation.h>
#include <yaml-cpp/yaml.h>

namespace clear {

MotionManager::MotionManager() : Node("MotionManager") {
  this->declare_parameter("/config_file", "");
}

MotionManager::~MotionManager() {
  run_.push(false);
  inner_loop_thread_.join();
}

void MotionManager::init() {
  intializationPtr_ =
      std::make_shared<Initialization>(this->shared_from_this());

  joyStickPtr_ = std::make_shared<JoyStick>(this->shared_from_this());

  while (!joyStickPtr_->isStart()) {
    rclcpp::spin_some(this->shared_from_this());
  }

  estimatorPtr_ =
      std::make_shared<StateEstimationLKF>(this->shared_from_this());

  gaitSchedulePtr_ = std::make_shared<GaitSchedule>(this->shared_from_this());

  trajGenPtr_ = std::make_shared<TrajectorGeneration>(this->shared_from_this());

  trajectoryStabilizationPtr_ =
      std::make_shared<TrajectoryStabilization>(this->shared_from_this());

  visPtr_ = std::make_shared<DataVisualization>(this->shared_from_this());

  const std::string config_file_ = this->get_parameter("/config_file")
                                       .get_parameter_value()
                                       .get<std::string>();

  auto config_ = YAML::LoadFile(config_file_);
  bool hardware_ = config_["estimation"]["hardware"].as<bool>();
  if (hardware_) {
    unitreeHWPtr_ = std::make_shared<UnitreeHW>(this->shared_from_this());
    scalar_t t0 = this->now().seconds();
    while (this->now().seconds() - t0 < 1.0) {
      unitreeHWPtr_->switch_to_damping();
      unitreeHWPtr_->read();
    }
  } else {
    intializationPtr_->reset_simulation();
    rclcpp::spin_some(this->shared_from_this());
  }

  inner_loop_thread_ = std::thread(&MotionManager::innerLoop, this);
  run_.push(true);
}

void MotionManager::innerLoop() {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  rclcpp::Rate loop_rate(500.0);
  const scalar_t ts = this->now().seconds();

  while (rclcpp::ok() && run_.get()) {

    if (joyStickPtr_->eStop()) {
      RCLCPP_INFO(this->get_logger(), "e-stop");
      trajGenPtr_.reset();
      trajectoryStabilizationPtr_.reset();
      visPtr_.reset();
      break;
    }

    if (gaitSchedulePtr_->getCurrentGaitName() == "stance" &&
        joyStickPtr_->isTrotting()) {
      gaitSchedulePtr_->switchGait("trot");
    } else if (gaitSchedulePtr_->getCurrentGaitName() == "trot" &&
               joyStickPtr_->isStance()) {
      gaitSchedulePtr_->switchGait("stance");
    }

    if (gaitSchedulePtr_->getCurrentGaitName() == "trot") {
      trajGenPtr_->setVelCmd(joyStickPtr_->getLinearVelCmd(),
                             joyStickPtr_->getYawVelCmd());
    }

    /* if (this->now().seconds() > ts + 6.0 &&
        gaitSchedulePtr_->getCurrentGaitName() != "trot") {
      gaitSchedulePtr_->switchGait("trot");
    }
    if (gaitSchedulePtr_->getCurrentGaitName() == "trot") {
      trajGenPtr_->setVelCmd(vector3_t(0.2, 0.0, 0.0), 0.1);
    } */

    // trajGenPtr_->setHeightCmd(joyStickPtr_->getHeightCmd());

    if (unitreeHWPtr_ != nullptr) {
      unitreeHWPtr_->read();
      estimatorPtr_->setImuMsg(unitreeHWPtr_->get_imu_msg());
      estimatorPtr_->setTouchMsg(unitreeHWPtr_->get_touch_msg());
      estimatorPtr_->setJointsMsg(unitreeHWPtr_->get_joint_msg());
    }

    scalar_t horizon_time_ =
        min(2.0, max(0.5, gaitSchedulePtr_->currentGaitCycle()));

    auto mode_schedule_ptr = gaitSchedulePtr_->eval(horizon_time_);

    trajGenPtr_->updateCurrentState(estimatorPtr_->getQpos(),
                                    estimatorPtr_->getQvel());

    trajGenPtr_->updateModeSchedule(mode_schedule_ptr);

    trajectoryStabilizationPtr_->updateCurrentState(estimatorPtr_->getQpos(),
                                                    estimatorPtr_->getQvel());

    trajectoryStabilizationPtr_->updateReferenceBuffer(
        trajGenPtr_->getReferenceBuffer());

    visPtr_->updateCurrentState(estimatorPtr_->getQpos(),
                                estimatorPtr_->getQvel());
    visPtr_->updateReferenceBuffer(trajGenPtr_->getReferenceBuffer());

    if (unitreeHWPtr_ != nullptr) {
      unitreeHWPtr_->set_actuator_cmds(trajectoryStabilizationPtr_->getCmds());
      unitreeHWPtr_->send();
    }

    loop_rate.sleep();
  }

  if (unitreeHWPtr_ != nullptr) {
    unitreeHWPtr_->switch_to_damping();
  }
  exit(0);
}

} // namespace clear
