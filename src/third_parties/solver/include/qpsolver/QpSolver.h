#pragma once

#include <core/types.h>

#include <blasfeo_d_aux_ext_dep.h>
#include <hpipm_d_dense_qp.h>
#include <hpipm_d_dense_qp_dim.h>
#include <hpipm_d_dense_qp_ipm.h>
#include <hpipm_d_dense_qp_sol.h>
#include <hpipm_d_dense_qp_utils.h>
#include <hpipm_timing.h>

namespace clear {

typedef d_dense_qp_dim DimsSpec;

typedef d_dense_qp_sol QpSolution;

typedef d_dense_qp_ipm_arg SolverConfig;

class QpSolver {

public:
  struct QpSolverSettings {
  public:
    ///
    /// @brief Solver mode. Default is hpipm_mode::SPEED.
    ///
    hpipm_mode mode = hpipm_mode::SPEED;

    ///
    /// @brief Maximum number of iterations. Must be nen-negative. Default
    /// is 15.
    ///
    int iter_max = 15;

    ///
    /// @brief Minimum step size. Must be positive and less than 1.0. Default
    /// is 1.0e-08.
    ///
    double alpha_min = 1.0e-08;

    ///
    /// @brief Initial barrier parameter. Must be positive. Default is 1.0e+02.
    ///
    double mu0 = 1.0e+02;

    ///
    /// @brief Convergence criteria. Must be positive. Default is 1.0e-08.
    ///
    double tol_stat = 1.0e-08;

    ///
    /// @brief Convergence criteria. Must be positive. Default is 1.0e-08.
    ///
    double tol_eq = 1.0e-08;

    ///
    /// @brief Convergence criteria. Must be positive. Default is 1.0e-08.
    ///
    double tol_ineq = 1.0e-08;

    ///
    /// @brief Convergence criteria. Must be positive. Default is 1.0e-08.
    ///
    double tol_comp = 1.0e-08; // convergence criteria

    ///
    /// @brief Regularization term. Must be non-negative. Default is 1.0e-12.
    ///
    double reg_prim = 1.0e-12;

    ///
    /// @brief Warm start flag (0: disable, 1: enable). Default is 0.
    ///
    int warm_start = 0;

    ///
    /// @brief Prediction-correction flag (0: disable, 1: enable). Default is 1.
    ///
    int pred_corr = 1;

    ///
    /// @brief Square-root Riccati flag (0: disable, 1: enable). Default is 1.
    ///
    int ric_alg = 1;

    ///
    /// @brief Use different step for primal and dual variables (0: disable, 1:
    /// enable). Default is 0.
    ///
    int split_step = 0;

    ///
    /// @brief Check the settings. If something is wrong, throws an exception.
    ///
    void checkSettings() const;
  };

public:
  QpSolver(const DimsSpec &dims);

  ~QpSolver();

  void update(Eigen::Ref<matrix_t> H, Eigen::Ref<vector_t> g,
              Eigen::Ref<matrix_t> A, Eigen::Ref<vector_t> b,
              Eigen::Ref<matrix_t> C, Eigen::Ref<vector_t> lb,
              Eigen::Ref<vector_t> ub);

  vector_t solve();

private:
  DimsSpec dims_;
  matrix_t H, A, C;
  vector_t g, b, lb, ub, sol;

  hpipm_size_t dim_size;
  struct d_dense_qp qp;
  void *dim_mem;
  void *qp_mem;

  hpipm_size_t qp_sol_size;
  void *qp_sol_mem;
  QpSolution qp_sol;

  hpipm_size_t ipm_arg_size;
  SolverConfig arg;
};

} // namespace clear
