#include "drake/systems/plants/inverseKinBackend.h"

#include <stdexcept>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Sparse>

#include "drake/core/Function.h"
#include "drake/core/Gradient.h"
#include "drake/solvers/Optimization.h"
#include "drake/systems/plants/constraint/RigidBodyConstraint.h"
#include "drake/systems/plants/ConstraintWrappers.h"
#include "drake/systems/plants/IKoptions.h"
#include "drake/systems/plants/RigidBodyTree.h"

using Eigen::Map;
using Eigen::MatrixBase;
using Eigen::MatrixXd;
using Eigen::VectorXd;
using Eigen::VectorXi;

using drake::solvers::SolutionResult;

namespace Drake {
namespace systems {
namespace plants {
namespace {

class Mode1Objective : public Constraint {
 public:
  // All references are aliased for the life of the objective.  It
  // should be short.
  Mode1Objective(RigidBodyTree* model, const MatrixXd& Q)
      : Constraint(model->num_positions),
        model_(model),
        Q_(Q) {}

  virtual void eval(const Eigen::Ref<const Eigen::VectorXd>& x,
                    Eigen::VectorXd& y) const override {
    VectorXd q_err = x - q_nom_i_;
    y(0) = q_err.transpose() * Q_ * q_err;
  }

  virtual void eval(const Eigen::Ref<const TaylorVecXd>& x,
                    TaylorVecXd& y) const override {
    VectorXd x_val = autoDiffToValueMatrix(x);
    VectorXd q_err = x_val - q_nom_i_;
    VectorXd y_val = q_err.transpose() * Q_ * q_err;
    MatrixXd dy_vec = 2 * q_err.transpose() * Q_;
    auto gradient_mat = autoDiffToGradientMatrix(x);
    initializeAutoDiffGivenGradientMatrix(
        y_val, (dy_vec * gradient_mat).eval(), y);
  }

  void set_q_nom(const VectorXd& q_nom_i) {
    q_nom_i_ = q_nom_i;
  }

 private:
  RigidBodyTree* model_;
  const MatrixXd& Q_;
  VectorXd q_nom_i_;
};

int GetSolverInfo(const OptimizationProblem& prog, SolutionResult result) {
  std::string solver_name;
  int solver_result = 0;
  prog.GetSolverResult(&solver_name, &solver_result);

  if (solver_name == "SNOPT") {
    // We can return SNOPT results directly.
    return solver_result;
  }

  // Make a SNOPT-like return code out of the generic result.
  switch (result) {
    case SolutionResult::kSolutionFound: {
      return 1;
    }
    case SolutionResult::kInvalidInput: {
      return 91;
    }
    case SolutionResult::kInfeasibleConstraints: {
      return 13;
    }
    case SolutionResult::kUnknownError: {
      return 100; // Not a real SNOPT error.
    }
  }

  return -1;
}

template <typename DerivedA, typename DerivedB, typename DerivedC,
          typename DerivedD, typename DerivedE>
void inverseKinMode1(
    RigidBodyTree* model, const int mode, const int nT,
    const double* t, const MatrixBase<DerivedA>& q_seed,
    const MatrixBase<DerivedB>& q_nom, const int num_constraints,
    RigidBodyConstraint** const constraint_array,
    const IKoptions& ikoptions, MatrixBase<DerivedC>* q_sol,
    MatrixBase<DerivedD>* qdot_sol, MatrixBase<DerivedE>* qddot_sol, int* INFO,
    std::vector<std::string>* infeasible_constraint) {

  OptimizationProblem prog;
  prog.SetSolverOption("SNOPT", "Major optimality tolerance",
                       ikoptions.getMajorOptimalityTolerance());
  prog.SetSolverOption("SNOPT", "Major feasibility tolerance",
                       ikoptions.getMajorFeasibilityTolerance());
  prog.SetSolverOption("SNOPT", "Superbasics limit",
                       ikoptions.getSuperbasicsLimit());
  prog.SetSolverOption("SNOPT", "Major iterations limit",
                       ikoptions.getMajorIterationsLimit());
  prog.SetSolverOption("SNOPT", "Iterations limit",
                       ikoptions.getIterationsLimit());

  DecisionVariableView vars = prog.AddContinuousVariables(model->num_positions);

  MatrixXd Q;
  ikoptions.getQ(Q);
  auto objective = std::make_shared<Mode1Objective>(model, Q);
  prog.AddCost(objective, {vars});

  bool qsc_active = false;
  for (int i = 0; i < num_constraints; i++) {
    RigidBodyConstraint* constraint = constraint_array[i];
    const int constraint_category = constraint->getCategory();
    if (constraint_category ==
        RigidBodyConstraint::SingleTimeKinematicConstraintCategory) {
      auto wrapper = std::make_shared<SingleTimeKinematicConstraintWrapper>(
          static_cast<SingleTimeKinematicConstraint*>(constraint));
      // TODO(sam.creasey) I believe that this should have the bounds
      // updated with changes in time steps.
      prog.AddGenericConstraint(wrapper, {vars});
    } else if (constraint_category ==
               RigidBodyConstraint::PostureConstraintCategory) {
      PostureConstraint* pc = static_cast<PostureConstraint*>(constraint);
      VectorXd lb;
      VectorXd ub;
      // TODO(sam.creasey) This needs to support updating the
      // posture constraints across time steps.  Possibly store an
      // array of PostureConstraint + BoundingBoxConstraint pairs and
      // add an update method to BoundingBoxConstraint.
      pc->bounds(&t[0], lb, ub);
      prog.AddBoundingBoxConstraint(lb, ub, {vars});
    } else if (constraint_category ==
               RigidBodyConstraint::SingleTimeLinearPostureConstraintCategory) {
      SingleTimeLinearPostureConstraint* st_lpc =
          static_cast<SingleTimeLinearPostureConstraint*>(constraint);
      // TODO(sam.creasey) This should have the constraints updated
      // across time steps.
      VectorXd lb;
      VectorXd ub;
      st_lpc->bounds(&t[0], lb, ub);

      VectorXi iAfun;
      VectorXi jAvar;
      VectorXd A;
      st_lpc->geval(&t[0], iAfun, jAvar, A);

      assert(iAfun.size() == jAvar.size());
      assert(iAfun.size() != A.size());

      typedef Eigen::Triplet<double> T;
      std::vector<T> triplet_list;
      for (size_t i = 0; i < iAfun.size(); i++) {
        triplet_list.push_back(T(iAfun[i], jAvar[i], A[i]));
      }

      Eigen::SparseMatrix<double> A_sparse(st_lpc->getNumConstraint(&t[0]),
                                           model->num_positions);
      A_sparse.setFromTriplets(triplet_list.begin(), triplet_list.end());
      prog.AddLinearConstraint(MatrixXd(A_sparse), lb, ub, {vars});
    } else if (constraint_category ==
               RigidBodyConstraint::QuasiStaticConstraintCategory) {
      if (qsc_active) {
        throw std::runtime_error(
            "Drake::inverseKinBackend: current implementation supports at "
            "most one QuasiStaticConstraint");
      }

      QuasiStaticConstraint* qsc =
          static_cast<QuasiStaticConstraint*>(constraint);
      int num_vars = qsc->getNumWeights();
      DecisionVariableView qsc_vars =
          prog.AddContinuousVariables(num_vars, "qsc");
      auto wrapper = std::make_shared<QuasiStaticConstraintWrapper>(qsc);
      prog.AddGenericConstraint(wrapper, {vars, qsc_vars});
      prog.AddBoundingBoxConstraint(VectorXd::Constant(num_vars, 0.),
                                    VectorXd::Constant(num_vars, 1.), {qsc_vars});
      VectorXd constraint(num_vars);
      constraint.fill(1.);
      prog.AddLinearEqualityConstraint(constraint.transpose(),
                                       Vector1d::Constant(1.), {qsc_vars});
      prog.SetInitialGuess(qsc_vars,
                           VectorXd::Constant(num_vars, 1.0 / num_vars));
    } else if (constraint_category ==
               RigidBodyConstraint::MultipleTimeKinematicConstraintCategory) {
      throw std::runtime_error("MultipleTimeKinematicConstraint is not supported"
                               " in pointwise mode.");
    } else if (constraint_category ==
               RigidBodyConstraint::MultipleTimeLinearPostureConstraintCategory) {
      throw std::runtime_error(
          "MultipleTimeLinearPostureConstraint is not supported"
          " in pointwise mode.");
    }
  }

  // Add a bounding box constraint from them model.
  prog.AddBoundingBoxConstraint(model->joint_limit_min, model->joint_limit_max,
                                {vars});

  for (int t = 0; t < nT; t++) {
    // TODO(sam.creasey) would this be faster if we stored the view
    // instead of copying into a VectorXd?
    objective->set_q_nom(q_nom.col(t));
    if (!ikoptions.getSequentialSeedFlag() || (t == 0)) {
      prog.SetInitialGuess(vars, q_seed.col(t));
    } else {
      prog.SetInitialGuess(vars, q_sol->col(t - 1));
    }

    SolutionResult result = prog.Solve();
    prog.PrintSolution();
    q_sol->col(t) = vars.value();
    INFO[t] = GetSolverInfo(prog, result);
  }
}

} // namespace (anon)

template <typename DerivedA, typename DerivedB, typename DerivedC,
          typename DerivedD, typename DerivedE>
void inverseKinBackend(
    RigidBodyTree* model, const int mode, const int nT,
    const double* t, const MatrixBase<DerivedA>& q_seed,
    const MatrixBase<DerivedB>& q_nom, const int num_constraints,
    RigidBodyConstraint** const constraint_array,
    const IKoptions& ikoptions, MatrixBase<DerivedC>* q_sol,
    MatrixBase<DerivedD>* qdot_sol, MatrixBase<DerivedE>* qddot_sol,
    int* INFO, std::vector<std::string>* infeasible_constraint) {

  // Validate some basic parameters of the input.
  if (q_seed.rows() != model->num_positions || q_seed.cols() != nT ||
      q_nom.rows() != model->num_positions || q_nom.cols() != nT) {
    throw std::runtime_error(
        "Drake::inverseKinBackend: q_seed and q_nom must be of size "
        "nq x nT");
  }

  // Check to see if the generic version supports the constraint types
  // we're using.
  bool constraints_supported = true;
  for (int i = 0; i < num_constraints; i++) {
    if ((constraint_array[i]->getCategory() !=
         RigidBodyConstraint::SingleTimeKinematicConstraintCategory) &&
        (constraint_array[i]->getCategory() !=
         RigidBodyConstraint::PostureConstraintCategory) &&
        (constraint_array[i]->getCategory() !=
         RigidBodyConstraint::SingleTimeLinearPostureConstraintCategory) &&
        (constraint_array[i]->getCategory() !=
         RigidBodyConstraint::QuasiStaticConstraintCategory)) {
      constraints_supported = false;
      break;
    }
  }

  // Fall through to SNOPT implementation if needed.
  if (!constraints_supported || (mode != 1) || (nT != 1)) {
    inverseKinSnoptBackend(model, mode, nT, t, q_seed,
                           q_nom, num_constraints, constraint_array,
                           ikoptions, q_sol, qdot_sol, qddot_sol, INFO,
                           infeasible_constraint);
    return;
  }

  // TODO(sam.creasey) This will need to be conditional when more than
  // one mode is supported...
  inverseKinMode1(model, mode, nT, t, q_seed,
                  q_nom, num_constraints, constraint_array,
                  ikoptions, q_sol, qdot_sol, qddot_sol, INFO,
                  infeasible_constraint);
  return;
}

template void inverseKinBackend(
    RigidBodyTree* model, const int mode, const int nT,
    const double* t, const MatrixBase<Map<MatrixXd>>& q_seed,
    const MatrixBase<Map<MatrixXd>>& q_nom, const int num_constraints,
    RigidBodyConstraint** const constraint_array,
    const IKoptions& ikoptions, MatrixBase<Map<MatrixXd>>* q_sol,
    MatrixBase<Map<MatrixXd>>* qdot_sol, MatrixBase<Map<MatrixXd>>* qddot_sol,
    int* INFO, std::vector<std::string>* infeasible_constraint);
template void inverseKinBackend(
    RigidBodyTree* model, const int mode, const int nT,
    const double* t, const MatrixBase<MatrixXd>& q_seed,
    const MatrixBase<MatrixXd>& q_nom, const int num_constraints,
    RigidBodyConstraint** const constraint_array,
    const IKoptions& ikoptions, MatrixBase<MatrixXd>* q_sol,
    MatrixBase<MatrixXd>* qdot_sol, MatrixBase<MatrixXd>* qddot_sol,
    int* INFO, std::vector<std::string>* infeasible_constraint);
template void inverseKinBackend(
    RigidBodyTree* model, const int mode, const int nT,
    const double* t, const MatrixBase<Map<MatrixXd>>& q_seed,
    const MatrixBase<Map<MatrixXd>>& q_nom, const int num_constraints,
    RigidBodyConstraint** const constraint_array,
    const IKoptions& ikoptions, MatrixBase<Map<MatrixXd>>* q_sol,
    MatrixBase<MatrixXd>* qdot_sol, MatrixBase<MatrixXd>* qddot_sol,
    int* INFO, std::vector<std::string>* infeasible_constraint);
template void inverseKinBackend(
    RigidBodyTree* model, const int mode, const int nT,
    const double* t, const MatrixBase<Map<VectorXd>>& q_seed,
    const MatrixBase<Map<VectorXd>>& q_nom, const int num_constraints,
    RigidBodyConstraint** const constraint_array,
    const IKoptions& ikoptions, MatrixBase<Map<VectorXd>>* q_sol,
    MatrixBase<Map<VectorXd>>* qdot_sol, MatrixBase<Map<VectorXd>>* qddot_sol,
    int* INFO, std::vector<std::string>* infeasible_constraint);
template void inverseKinBackend(
    RigidBodyTree* model, const int mode, const int nT,
    const double* t, const MatrixBase<VectorXd>& q_seed,
    const MatrixBase<VectorXd>& q_nom, const int num_constraints,
    RigidBodyConstraint** const constraint_array,
    const IKoptions& ikoptions, MatrixBase<VectorXd>* q_sol,
    MatrixBase<VectorXd>* qdot_sol, MatrixBase<VectorXd>* qddot_sol,
    int* INFO, std::vector<std::string>* infeasible_constraint);
template void inverseKinBackend(
    RigidBodyTree* model, const int mode, const int nT,
    const double* t, const MatrixBase<Map<VectorXd>>& q_seed,
    const MatrixBase<Map<VectorXd>>& q_nom, const int num_constraints,
    RigidBodyConstraint** const constraint_array,
    const IKoptions& ikoptions, MatrixBase<Map<VectorXd>>* q_sol,
    MatrixBase<VectorXd>* qdot_sol, MatrixBase<VectorXd>* qddot_sol,
    int* INFO, std::vector<std::string>* infeasible_constraint);
}
}
}
