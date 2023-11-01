#include "generation/FootholdOptimization.h"
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <yaml-cpp/yaml.h>

namespace clear {

FootholdOptimization::FootholdOptimization(
    std::string config_yaml,
    std::shared_ptr<PinocchioInterface> pinocchioInterface_ptr,
    std::shared_ptr<TrajectoriesArray> referenceTrajectoriesBuffer)
    : pinocchioInterface_ptr_(pinocchioInterface_ptr),
      referenceTrajectoriesBuffer_(referenceTrajectoriesBuffer) {

  auto config_ = YAML::LoadFile(config_yaml);

  foot_names = config_["model"]["foot_names"].as<std::vector<std::string>>();
  base_name = config_["model"]["base_name"].as<std::string>();

  vector_t qpos, qvel;
  qpos.setZero(pinocchioInterface_ptr_->nq());
  qvel.setZero(pinocchioInterface_ptr_->nv());
  pinocchioInterface_ptr_->updateRobotState(qpos, qvel);
  for (const auto &foot : foot_names) {
    footholds_nominal_pos[foot] =
        pinocchioInterface_ptr_->getFramePose(foot).translation();
    footholds_nominal_pos[foot].z() = 0.0;
    footholds_nominal_pos[foot].y() *= 0.8;
  }

  nf = foot_names.size();
}

FootholdOptimization::~FootholdOptimization() {}

std::map<std::string, std::pair<scalar_t, vector3_t>>
FootholdOptimization::optimize(
    scalar_t t, const std::shared_ptr<ModeSchedule> mode_schedule) {
  footholds_ = std::map<std::string, std::pair<scalar_t, vector3_t>>();
  auto base_pos_ref_traj =
      referenceTrajectoriesBuffer_.get()->get_base_pos_ref_traj();
  auto base_rpy_traj = referenceTrajectoriesBuffer_.get()->get_base_rpy_traj();
  if (mode_schedule.get() == nullptr || base_pos_ref_traj.get() == nullptr ||
      base_rpy_traj.get() == nullptr) {
    return footholds_;
  }
  heuristic(t, mode_schedule);
  return footholds_;
}

void FootholdOptimization::heuristic(
    scalar_t t, const std::shared_ptr<ModeSchedule> mode_schedule) {

  auto base_pos_ref_traj =
      referenceTrajectoriesBuffer_.get()->get_base_pos_ref_traj();
  auto base_rpy_traj = referenceTrajectoriesBuffer_.get()->get_base_rpy_traj();

  vector3_t v_des = base_pos_ref_traj->derivative(t, 1);

  const auto base_pose = pinocchioInterface_ptr_->getFramePose(base_name);
  const auto base_twist =
      pinocchioInterface_ptr_->getFrame6dVel_localWorldAligned(base_name);

  vector3_t rpy_dot =
      getJacobiFromOmegaToRPY(toEulerAngles(base_pose.rotation())) *
      base_twist.angular();

  auto swtr = quadruped::getTimeOfNextTouchDown(0, mode_schedule);

  const scalar_t p_rel_x_max = 0.5f;
  const scalar_t p_rel_y_max = 0.4f;

  for (size_t i = 0; i < nf; i++) {
    const auto &foot_name = foot_names[i];
    const scalar_t nextStanceTime = swtr[i];

    vector3_t pYawCorrected =
        toRotationMatrix(base_rpy_traj->evaluate(t + nextStanceTime)) *
        footholds_nominal_pos[foot_name];

    scalar_t pfx_rel, pfy_rel;
    std::pair<scalar_t, vector3_t> foothold;
    foothold.first = nextStanceTime + t;
    foothold.second =
        base_pose.translation() +
        (pYawCorrected + std::max(0.0, nextStanceTime) * base_twist.linear());
    pfx_rel = 0.5 * mode_schedule->duration() * v_des.x() +
              0.1 * (base_twist.linear().x() - v_des.x()) +
              (0.5 * base_pose.translation().z() / 9.81) *
                  (base_twist.linear().y() * rpy_dot.z());
    pfy_rel = 0.5 * mode_schedule->duration() * v_des.y() +
              0.1 * (base_twist.linear().y() - v_des.y()) +
              (0.5 * base_pose.translation().z() / 9.81) *
                  (-base_twist.linear().x() * rpy_dot.z());
    pfx_rel = std::min(std::max(pfx_rel, -p_rel_x_max), p_rel_x_max);
    pfy_rel = std::min(std::max(pfy_rel, -p_rel_y_max), p_rel_y_max);
    foothold.second.x() += pfx_rel;
    foothold.second.y() += pfy_rel;
    foothold.second.z() = 0.02;
    footholds_[foot_name] = foothold;
  }
}

} // namespace clear