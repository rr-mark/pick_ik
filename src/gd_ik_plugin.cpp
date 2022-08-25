#include <gd_ik/algorithm.hpp>
#include <gd_ik/frame.hpp>
#include <gd_ik/goal.hpp>
#include <gd_ik/robot.hpp>

#include <gd_ik_parameters.hpp>
#include <pluginlib/class_list_macros.hpp>
#include <rclcpp/rclcpp.hpp>

#include <moveit/kinematics_base/kinematics_base.h>
#include <moveit/robot_model/joint_model_group.h>
#include <moveit/robot_state/robot_state.h>
#include <string>
#include <vector>

namespace gd_ik {
namespace {
auto const LOGGER = rclcpp::get_logger("gd_ik");
}

class GDIKPlugin : public kinematics::KinematicsBase {
  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<ParamListener> parameter_listener_;
  Params params_;
  moveit::core::JointModelGroup const* jmg_;

  std::vector<std::string> joint_names_;
  std::vector<std::string> link_names_;
  Robot robot_;
  std::vector<size_t> tip_link_indexes_;
  std::vector<size_t> active_variable_indexes_;
  std::vector<double> minimal_displacement_factors_;

 public:
  virtual bool searchPositionIK(
      std::vector<geometry_msgs::msg::Pose> const& ik_poses,
      std::vector<double> const& ik_seed_state, double timeout,
      std::vector<double> const& consistency_limits,
      std::vector<double>& solution, IKCallbackFn const& solution_callback,
      IKCostFn cost_function, moveit_msgs::msg::MoveItErrorCodes& error_code,
      kinematics::KinematicsQueryOptions const& options =
          kinematics::KinematicsQueryOptions(),
      moveit::core::RobotState const* context_state = nullptr) const {
    // If we didn't receive a robot state we have to make one
    std::unique_ptr<moveit::core::RobotState> temp_state;
    if (!context_state) {
      temp_state = std::make_unique<moveit::core::RobotState>(robot_model_);
      temp_state->setToDefaultValues();
      context_state = temp_state.get();
    }

    auto const goal_frames =
        transform_poses_to_frames(*context_state, ik_poses, getBaseFrame());
    auto const frame_tests =
        make_frame_tests(goal_frames, params_.position_threshold,
                         params_.rotation_threshold, params_.twist_threshold);
    auto const initial_guess = get_variables(*context_state);
    auto const active_initial_guess =
        select(initial_guess, active_variable_indexes_);

    // Create goals
    auto goals = std::vector<Goal>{};
    if (params_.center_joints_weight > 0.0) {
      goals.push_back(
          Goal{make_center_joints_cost_fn(robot_, active_variable_indexes_,
                                          minimal_displacement_factors_),
               params_.center_joints_weight});
    }
    if (params_.avoid_joint_limits_weight > 0.0) {
      goals.push_back(
          Goal{make_avoid_joint_limits_cost_fn(robot_, active_variable_indexes_,
                                               minimal_displacement_factors_),
               params_.avoid_joint_limits_weight});
    }
    if (params_.minimal_displacement_weight > 0.0) {
      goals.push_back(
          Goal{make_minimal_displacement_cost_fn(active_initial_guess,
                                                 minimal_displacement_factors_),
               params_.minimal_displacement_weight});
    }
    if (cost_function) {
      for (auto const& pose : ik_poses) {
        goals.push_back(Goal{make_ik_cost_fn(pose, cost_function, robot_model_,
                                             jmg_, initial_guess),
                             1.0});
      }
    }

    auto const solution_test =
        make_is_solution_test_fn(frame_tests, goals, params_.cost_threshold);

    return false;
  }

  // kinematics::KinematicsBase ////////////////////////////////////////////////
  bool initialize(rclcpp::Node::SharedPtr const& node,
                  moveit::core::RobotModel const& robot_model,
                  std::string const& group_name, std::string const& base_frame,
                  std::vector<std::string> const& tip_frames,
                  double search_discretization) override {
    node_ = node;
    parameter_listener_ = std::make_shared<ParamListener>(
        node, std::string("robot_description_kinematics.").append(group_name));
    params_ = parameter_listener_->get_params();

    // Initialize internal state of base class KinematicsBase
    // Creates these internal state variables:
    // robot_model_ <- shared_ptr to RobotModel
    // robot_description_ <- empty string
    // group_name_ <- group_name string
    // base_frame_ <- base_frame without leading /
    // tip_frames_ <- tip_frames without leading /
    // redundant_joint_discretization_ <- vector initialized with
    // search_discretization
    storeValues(robot_model, group_name, base_frame, tip_frames,
                search_discretization);

    // Initialize internal state
    jmg_ = robot_model_->getJointModelGroup(group_name);
    if (!jmg_) {
      RCLCPP_ERROR(LOGGER, "failed to get joint model group %s",
                   group_name.c_str());
      return false;
    }

    // Joint names come from jmg
    for (auto* joint_model : jmg_->getJointModels()) {
      if (joint_model->getName() != base_frame_ &&
          joint_model->getType() != moveit::core::JointModel::UNKNOWN &&
          joint_model->getType() != moveit::core::JointModel::FIXED) {
        joint_names_.push_back(joint_model->getName());
      }
    }

    // If jmg has tip frames, set tip_frames_ to jmg tip frames
    // consider removing these lines as they might be unnecessary
    // as tip_frames_ is set by the call to storeValues above
    auto jmg_tips = std::vector<std::string>{};
    jmg_->getEndEffectorTips(jmg_tips);
    if (!jmg_tips.empty()) tip_frames_ = jmg_tips;
    ///////////////////////////////////////////////////////////

    // link_names are the same as tip frames
    // TODO: why do we need to set this
    link_names_ = tip_frames_;

    // Create our internal Robot object from the robot model
    robot_ = Robot::from(robot_model_);

    // Calculate internal state used in IK
    tip_link_indexes_ = get_link_indexes(robot_model_, tip_frames_);
    active_variable_indexes_ =
        get_active_variable_indexes(robot_model_, jmg_, tip_link_indexes_);
    minimal_displacement_factors_ =
        get_minimal_displacement_factors(active_variable_indexes_, robot_);

    return false;
  }

  virtual std::vector<std::string> const& getJointNames() const {
    return joint_names_;
  }

  virtual std::vector<std::string> const& getLinkNames() const {
    return link_names_;
  }

  virtual bool getPositionFK(
      std::vector<std::string> const& link_names,
      std::vector<double> const& joint_angles,
      std::vector<geometry_msgs::msg::Pose>& poses) const {
    return false;
  }

  virtual bool getPositionIK(geometry_msgs::msg::Pose const& ik_pose,
                             std::vector<double> const& ik_seed_state,
                             std::vector<double>& solution,
                             moveit_msgs::msg::MoveItErrorCodes& error_code,
                             kinematics::KinematicsQueryOptions const& options =
                                 kinematics::KinematicsQueryOptions()) const {
    return false;
  }

  virtual bool searchPositionIK(
      geometry_msgs::msg::Pose const& ik_pose,
      std::vector<double> const& ik_seed_state, double timeout,
      std::vector<double>& solution,
      moveit_msgs::msg::MoveItErrorCodes& error_code,
      kinematics::KinematicsQueryOptions const& options =
          kinematics::KinematicsQueryOptions()) const {
    return searchPositionIK(std::vector<geometry_msgs::msg::Pose>{ik_pose},
                            ik_seed_state, timeout, std::vector<double>(),
                            solution, IKCallbackFn(), error_code, options);
  }

  virtual bool searchPositionIK(
      geometry_msgs::msg::Pose const& ik_pose,
      std::vector<double> const& ik_seed_state, double timeout,
      std::vector<double> const& consistency_limits,
      std::vector<double>& solution,
      moveit_msgs::msg::MoveItErrorCodes& error_code,
      kinematics::KinematicsQueryOptions const& options =
          kinematics::KinematicsQueryOptions()) const {
    return searchPositionIK(std::vector<geometry_msgs::msg::Pose>{ik_pose},
                            ik_seed_state, timeout, consistency_limits,
                            solution, IKCallbackFn(), error_code, options);
  }

  virtual bool searchPositionIK(
      geometry_msgs::msg::Pose const& ik_pose,
      std::vector<double> const& ik_seed_state, double timeout,
      std::vector<double>& solution, IKCallbackFn const& solution_callback,
      moveit_msgs::msg::MoveItErrorCodes& error_code,
      kinematics::KinematicsQueryOptions const& options =
          kinematics::KinematicsQueryOptions()) const {
    return searchPositionIK(std::vector<geometry_msgs::msg::Pose>{ik_pose},
                            ik_seed_state, timeout, std::vector<double>(),
                            solution, solution_callback, error_code, options);
  }

  virtual bool searchPositionIK(
      geometry_msgs::msg::Pose const& ik_pose,
      std::vector<double> const& ik_seed_state, double timeout,
      std::vector<double> const& consistency_limits,
      std::vector<double>& solution, IKCallbackFn const& solution_callback,
      moveit_msgs::msg::MoveItErrorCodes& error_code,
      kinematics::KinematicsQueryOptions const& options =
          kinematics::KinematicsQueryOptions()) const {
    return searchPositionIK(std::vector<geometry_msgs::msg::Pose>{ik_pose},
                            ik_seed_state, timeout, consistency_limits,
                            solution, solution_callback, error_code, options);
  }

  virtual bool searchPositionIK(
      std::vector<geometry_msgs::msg::Pose> const& ik_poses,
      std::vector<double> const& ik_seed_state, double timeout,
      std::vector<double> const& consistency_limits,
      std::vector<double>& solution, IKCallbackFn const& solution_callback,
      moveit_msgs::msg::MoveItErrorCodes& error_code,
      kinematics::KinematicsQueryOptions const& options =
          kinematics::KinematicsQueryOptions(),
      moveit::core::RobotState const* context_state = NULL) const {
    return searchPositionIK(ik_poses, ik_seed_state, timeout,
                            consistency_limits, solution, solution_callback,
                            IKCostFn(), error_code, options, context_state);
  }
};

}  // namespace gd_ik

PLUGINLIB_EXPORT_CLASS(gd_ik::GDIKPlugin, kinematics::KinematicsBase);
