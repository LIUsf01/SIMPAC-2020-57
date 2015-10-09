/**
  This code is licensed under the "GNU GPL version 2 or later". See
  LICENSE file or https://www.gnu.org/licenses/gpl-2.0.html

  Copyright 2013-2015: Thomas Wick and Timo Heister
*/

// Geomechanics: Crack with phase-field
// monolithic approach and a primal dual active set strategy
// Predictor-corrector mesh adaptivity
// 2d code version

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/logstream.h>
#include <deal.II/base/function.h>
#include <deal.II/base/utilities.h>
#include <deal.II/base/timer.h>
#include <deal.II/base/parameter_handler.h>
#include <deal.II/base/function_parser.h>

#include <deal.II/lac/block_vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/block_sparse_matrix.h>
#include <deal.II/lac/sparse_direct.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/lac/solver_gmres.h>

#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/grid/tria_boundary_lib.h>
#include <deal.II/grid/grid_tools.h>
#include <deal.II/grid/grid_in.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_dgq.h>
#include <deal.II/fe/fe_dgp.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/mapping_q1.h>
#include <deal.II/numerics/error_estimator.h>

#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/solution_transfer.h>

#include <deal.II/lac/generic_linear_algebra.h>
namespace LA
{
  using namespace dealii::LinearAlgebraTrilinos;
}
#include <deal.II/distributed/tria.h>
#include <deal.II/distributed/grid_refinement.h>
#include <deal.II/distributed/solution_transfer.h>

#include <fstream>
#include <sstream>

using namespace dealii;


// For Example 3 (multiple cracks in a heterogenous medium)
// reads .pgm file and returns it as floating point values
// taken from step-42
class BitmapFile
{
public:
  BitmapFile(const std::string &name);

  double
  get_value(const double x, const double y) const;

private:
  std::vector<double> image_data;
  double hx, hy;
  int nx, ny;

  double
  get_pixel_value(const int i, const int j) const;
};

// The constructor of this class reads in the data that describes
// the obstacle from the given file name.
BitmapFile::BitmapFile(const std::string &name)
  :
  image_data(0),
  hx(0),
  hy(0),
  nx(0),
  ny(0)
{
  std::ifstream f(name.c_str());
  AssertThrow (f, ExcMessage (std::string("Can't read from file <") +
                              name + ">!"));

  std::string temp;
  getline(f, temp);
  f >> temp;
  if (temp[0]=='#')
    getline(f, temp);

  f >> nx >> ny;

  AssertThrow(nx > 0 && ny > 0, ExcMessage("Invalid file format."));

  for (int k = 0; k < nx * ny; k++)
    {
      unsigned int val;
      f >> val;
      image_data.push_back(val / 255.0);
    }

  hx = 1.0 / (nx - 1);
  hy = 1.0 / (ny - 1);
}

// The following two functions return the value of a given pixel with
// coordinates $i,j$, which we identify with the values of a function
// defined at positions <code>i*hx, j*hy</code>, and at arbitrary
// coordinates $x,y$ where we do a bilinear interpolation between
// point values returned by the first of the two functions. In the
// second function, for each $x,y$, we first compute the (integer)
// location of the nearest pixel coordinate to the bottom left of
// $x,y$, and then compute the coordinates $\xi,\eta$ within this
// pixel. We truncate both kinds of variables from both below
// and above to avoid problems when evaluating the function outside
// of its defined range as may happen due to roundoff errors.
double
BitmapFile::get_pixel_value(const int i,
                            const int j) const
{
  assert(i >= 0 && i < nx);
  assert(j >= 0 && j < ny);
  return image_data[nx * (ny - 1 - j) + i];
}

double
BitmapFile::get_value(const double x,
                      const double y) const
{
  const int ix = std::min(std::max((int) (x / hx), 0), nx - 2);
  const int iy = std::min(std::max((int) (y / hy), 0), ny - 2);

  const double xi  = std::min(std::max((x-ix*hx)/hx, 1.), 0.);
  const double eta = std::min(std::max((y-iy*hy)/hy, 1.), 0.);

  return ((1-xi)*(1-eta)*get_pixel_value(ix,iy)
          +
          xi*(1-eta)*get_pixel_value(ix+1,iy)
          +
          (1-xi)*eta*get_pixel_value(ix,iy+1)
          +
          xi*eta*get_pixel_value(ix+1,iy+1));
}

template <int dim>
class BitmapFunction : public Function<dim>
{
public:
  BitmapFunction(const std::string &filename,
                 double x1_, double x2_, double y1_, double y2_, double minvalue_, double maxvalue_)
    : Function<dim>(1),
      f(filename), x1(x1_), x2(x2_), y1(y1_), y2(y2_), minvalue(minvalue_), maxvalue(maxvalue_)
  {}

  virtual
  double value (const Point<dim> &p,
                const unsigned int /*component*/) const
  {
    Assert(dim==2, ExcNotImplemented());
    double x = (p(0)-x1)/(x2-x1);
    double y = (p(1)-y1)/(y2-y1);
    return minvalue + f.get_value(x,y)*(maxvalue-minvalue);
  }
private:
  BitmapFile f;
  double x1,x2,y1,y2;
  double minvalue, maxvalue;
};



// Define some tensors for cleaner notation later.
namespace Tensors
{

  template <int dim>
  inline Tensor<1, dim>
  get_grad_pf (
    unsigned int q,
    const std::vector<std::vector<Tensor<1, dim> > > &old_solution_grads)
  {
    Tensor<1, dim> grad_pf;
    grad_pf[0] = old_solution_grads[q][dim][0];
    grad_pf[1] = old_solution_grads[q][dim][1];

    return grad_pf;
  }

  template <int dim>
  inline Tensor<2, dim>
  get_grad_u (
    unsigned int q,
    const std::vector<std::vector<Tensor<1, dim> > > &old_solution_grads)
  {
    Tensor<2, dim> structure_continuation;
    structure_continuation[0][0] = old_solution_grads[q][0][0];
    structure_continuation[0][1] = old_solution_grads[q][0][1];
    structure_continuation[1][0] = old_solution_grads[q][1][0];
    structure_continuation[1][1] = old_solution_grads[q][1][1];

    return structure_continuation;
  }

  template <int dim>
  inline Tensor<2, dim>
  get_Identity ()
  {
    Tensor<2, dim> identity;
    identity[0][0] = 1.0;
    identity[0][1] = 0.0;
    identity[1][0] = 0.0;
    identity[1][1] = 1.0;

    return identity;
  }

  template <int dim>
  inline Tensor<1, dim>
  get_u (
    unsigned int q,
    const std::vector<Vector<double> > &old_solution_values)
  {
    Tensor<1, dim> u;
    u[0] = old_solution_values[q](0);
    u[1] = old_solution_values[q](1);

    return u;
  }

  template <int dim>
  inline Tensor<1, dim>
  get_u_LinU (
    const Tensor<1, dim> &phi_i_u)
  {
    Tensor<1, dim> tmp;
    tmp[0] = phi_i_u[0];
    tmp[1] = phi_i_u[1];

    return tmp;
  }

}



// Several classes for initial (phase-field) values
// Here, we prescribe initial (multiple) cracks
template <int dim>
class InitialValuesSneddon : public Function<dim>
{
public:
  InitialValuesSneddon (
    const double min_cell_diameter)
    :
    Function<dim>(dim+1)
  {
    _min_cell_diameter = min_cell_diameter;
  }

  virtual double
  value (
    const Point<dim> &p, const unsigned int component = 0) const;

  virtual void
  vector_value (
    const Point<dim> &p, Vector<double> &value) const;

private:
  double _min_cell_diameter;

};

template <int dim>
double
InitialValuesSneddon<dim>::value (
  const Point<dim> &p, const unsigned int component) const
{
  double width = _min_cell_diameter;
  double height = _min_cell_diameter;///2.0;
  double top = 2.0 + height;
  double bottom = 2.0 - height;
  // Defining the initial crack(s)
  // 0 = crack
  // 1 = no crack
  if (component == dim)
    {
      if (((p(0) >= 1.8 - width) && (p(0) <= 2.2 + width))
          && ((p(1) >= bottom) && (p(1) <= top)))
        return 0.0;
      else
        return 1.0;
    }

  return 0.0;
}

template <int dim>
void
InitialValuesSneddon<dim>::vector_value (
  const Point<dim> &p, Vector<double> &values) const
{
  for (unsigned int comp = 0; comp < this->n_components; ++comp)
    values(comp) = InitialValuesSneddon<dim>::value(p, comp);
}

template <int dim>
class ExactPhiSneddon : public Function<dim>
{
public:
  ExactPhiSneddon (const double eps_)
    :
    Function<dim>(dim+1),
    eps(eps_)
  {
  }

  virtual double
  value (
    const Point<dim> &p, const unsigned int component = 0) const
  {
    double dist = 0.0;
    Point<dim> left(1.8, 2.0);
    Point<dim> right(2.2, 2.0);

    if (p(0)<1.8)
      dist = left.distance(p);
    else if (p(0)>2.2)
      dist = right.distance(p);
    else
      dist=std::abs(p(1)-2.0);
    return 1.0 - exp(-dist/eps);
  }

private:
  double eps;
};


template <int dim>
class SneddonExactPostProc : public DataPostprocessor<dim>
{
public:
  SneddonExactPostProc (const double eps)
    : exact(eps)
  {}

  void compute_derived_quantities_vector (const std::vector<Vector<double> >              &uh,
                                          const std::vector<std::vector<Tensor<1,dim> > > &duh,
                                          const std::vector<std::vector<Tensor<2,dim> > > &dduh,
                                          const std::vector<Point<dim> >                  &normals,
                                          const std::vector<Point<dim> >                   &evaluation_points,
                                          std::vector<Vector<double> >                    &computed_quantities) const

  {
    for (unsigned int i=0; i<computed_quantities.size(); ++i)
      computed_quantities[i][0] = exact.value(evaluation_points[i]);
  }

  virtual std::vector<std::string> get_names () const
  {
    std::vector<std::string> r;
    r.push_back("exact_phi");
    return r;
  }

  virtual
  std::vector<DataComponentInterpretation::DataComponentInterpretation>
  get_data_component_interpretation () const
  {
    std::vector<DataComponentInterpretation::DataComponentInterpretation> r;
    r.push_back(DataComponentInterpretation::component_is_scalar);
    return r;
  }
  virtual UpdateFlags get_needed_update_flags () const
  {
    return  update_q_points;
  }
private:
  ExactPhiSneddon<dim> exact;
};



// Class for initial values multiple fractures in a homogeneous material
template <int dim>
class InitialValuesMultipleHomo : public Function<dim>
{
public:
  InitialValuesMultipleHomo (
    const double min_cell_diameter)
    :
    Function<dim>(dim+1)
  {
    _min_cell_diameter = min_cell_diameter;
  }

  virtual double
  value (
    const Point<dim> &p, const unsigned int component = 0) const;

  virtual void
  vector_value (
    const Point<dim> &p, Vector<double> &value) const;

private:
  double _min_cell_diameter;

};

template <int dim>
double
InitialValuesMultipleHomo<dim>::value (
  const Point<dim> &p, const unsigned int component) const
{
  double width = _min_cell_diameter;
  double height = _min_cell_diameter;
  // Defining the initial crack(s)
  // 0 = crack
  // 1 = no crack
  bool example_3 = true;
  if (component == dim)
    {
      if (example_3)
        {
          // Example 3 of our paper
          if (((p(0) >= 2.5 - width/2.0) && (p(0) <= 2.5 + width/2.0))
              && ((p(1) >= 0.8) && (p(1) <= 1.5)))
            return 0.0;
          else if (((p(0) >= 0.5) && (p(0) <= 1.5))
                   && ((p(1) >= 3.0 - height/2.0) && (p(1) <= 3.0 + height/2.0)))
            return 0.0;
          else
            return 1.0;
        }
      else
        {
          // Two parallel fractures
          if (((p(0) >= 1.6 - width) && (p(0) <= 2.4 + width))
              && ((p(1) >= 2.75 - height) && (p(1) <= 2.75 + height)))
            return 0.0;
          else if (((p(0) >= 1.6 - width) && (p(0) <= 2.4 + width))
                   && ((p(1) >= 1.25 - height) && (p(1) <= 1.25 + height)))
            return 0.0;
          else
            return 1.0;

        }
    }

  return 0.0;
}

template <int dim>
void
InitialValuesMultipleHomo<dim>::vector_value (
  const Point<dim> &p, Vector<double> &values) const
{
  for (unsigned int comp = 0; comp < this->n_components; ++comp)
    values(comp) = InitialValuesMultipleHomo<dim>::value(p, comp);
}






// Class for initial values multiple fractures in a heterogeneous material
template <int dim>
class InitialValuesMultipleHet : public Function<dim>
{
public:
  InitialValuesMultipleHet (
    const double min_cell_diameter)
    :
    Function<dim>(dim+1)
  {
    _min_cell_diameter = min_cell_diameter;
  }

  virtual double
  value (
    const Point<dim> &p, const unsigned int component = 0) const;

  virtual void
  vector_value (
    const Point<dim> &p, Vector<double> &value) const;

private:
  double _min_cell_diameter;

};

template <int dim>
double
InitialValuesMultipleHet<dim>::value (
  const Point<dim> &p, const unsigned int component) const
{
  double width = _min_cell_diameter;
  double height = _min_cell_diameter;
  // Defining the initial crack(s)
  // 0 = crack
  // 1 = no crack
  bool example_3 = true;
  if (component == dim)
    {
      if (example_3)
        {
          // Example 3 of our paper
          if (((p(0) >= 2.5 - width/2.0) && (p(0) <= 2.5 + width/2.0))
              && ((p(1) >= 0.8) && (p(1) <= 1.5)))
            return 0.0;
          else if (((p(0) >= 0.5) && (p(0) <= 1.5))
                   && ((p(1) >= 3.0 - height/2.0) && (p(1) <= 3.0 + height/2.0)))
            return 0.0;
          else
            return 1.0;
        }
      else
        {
          // Two parallel fractures
          if (((p(0) >= 1.6 - width) && (p(0) <= 2.4 + width))
              && ((p(1) >= 2.75 - height) && (p(1) <= 2.75 + height)))
            return 0.0;
          else if (((p(0) >= 1.6 - width) && (p(0) <= 2.4 + width))
                   && ((p(1) >= 1.25 - height) && (p(1) <= 1.25 + height)))
            return 0.0;
          else
            return 1.0;

        }
    }

  return 0.0;
}

template <int dim>
void
InitialValuesMultipleHet<dim>::vector_value (
  const Point<dim> &p, Vector<double> &values) const
{
  for (unsigned int comp = 0; comp < this->n_components; ++comp)
    values(comp) = InitialValuesMultipleHet<dim>::value(p, comp);
}


template <int dim>
class InitialValuesMiehe : public Function<dim>
{
public:
  InitialValuesMiehe (
    const double min_cell_diameter)
    :
    Function<dim>(dim+1)
  {
    _min_cell_diameter = min_cell_diameter;
  }

  virtual double
  value (
    const Point<dim> &p, const unsigned int component = 0) const;

  virtual void
  vector_value (
    const Point<dim> &p, Vector<double> &value) const;

private:
  double _min_cell_diameter;

};

template <int dim>
double
InitialValuesMiehe<dim>::value (
  const Point<dim> & /*p*/, const unsigned int component) const
{
  // Defining the initial crack(s)
  // 0 = crack
  // 1 = no crack
  if (component == dim)
    {
      return 1.0;
    }

  return 0.0;
}

template <int dim>
void
InitialValuesMiehe<dim>::vector_value (
  const Point<dim> &p, Vector<double> &values) const
{
  for (unsigned int comp = 0; comp < this->n_components; ++comp)
    values(comp) = InitialValuesMiehe<dim>::value(p, comp);
}


// Several classes for Dirichlet boundary conditions
// for displacements for the single-edge notched test (Miehe 2010)
// Example 2a (Miehe tension)
template <int dim>
class BoundaryParabelTension : public Function<dim>
{
public:
  BoundaryParabelTension (const double time)
    : Function<dim>(dim+1)
  {
    _time = time;
  }

  virtual double value (const Point<dim>   &p,
                        const unsigned int  component = 0) const;

  virtual void vector_value (const Point<dim> &p,
                             Vector<double>   &value) const;

private:
  double _time;

};

// The boundary values are given to component
// with number 0.
template <int dim>
double
BoundaryParabelTension<dim>::value (const Point<dim>  &p,
                                    const unsigned int component) const
{
  Assert (component < this->n_components,
          ExcIndexRange (component, 0, this->n_components));


  double dis_step_per_timestep = 1.0;

  if (component == 1)
    {
      return ( ((p(1) == 1.0) && (p(0) <= 1.0) && (p(0) >= 0.0))
               ?
               (1.0) * _time *dis_step_per_timestep : 0 );

    }



  return 0;
}



template <int dim>
void
BoundaryParabelTension<dim>::vector_value (const Point<dim> &p,
                                           Vector<double>   &values) const
{
  for (unsigned int c=0; c<this->n_components; ++c)
    values (c) = BoundaryParabelTension<dim>::value (p, c);
}




// Dirichlet boundary conditions for
// Miehe's et al. shear test
// Example 2b
template <int dim>
class BoundaryParabelShear : public Function<dim>
{
public:
  BoundaryParabelShear (const double time)
    : Function<dim>(dim+1)
  {
    _time = time;
  }

  virtual double value (const Point<dim>   &p,
                        const unsigned int  component = 0) const;

  virtual void vector_value (const Point<dim> &p,
                             Vector<double>   &value) const;

private:
  double _time;

};

// The boundary values are given to component
// with number 0.
template <int dim>
double
BoundaryParabelShear<dim>::value (const Point<dim>  &p,
                                  const unsigned int component) const
{
  Assert (component < this->n_components,
          ExcIndexRange (component, 0, this->n_components));


  double dis_step_per_timestep = -1.0;

  if (component == 0)
    {
      return ( ((p(1) == 1.0) )
               ?
               (1.0) * _time *dis_step_per_timestep : 0 );

    }


  return 0;
}



template <int dim>
void
BoundaryParabelShear<dim>::vector_value (const Point<dim> &p,
                                         Vector<double>   &values) const
{
  for (unsigned int c=0; c<this->n_components; ++c)
    values (c) = BoundaryParabelShear<dim>::value (p, c);
}




// Main program
template <int dim>
class FracturePhaseFieldProblem
{
public:

  FracturePhaseFieldProblem (
    const unsigned int degree, ParameterHandler &);
  void
  run ();
  static void
  declare_parameters (ParameterHandler &prm);

private:

  void
  set_runtime_parameters ();
  void
  determine_mesh_dependent_parameters();
  void
  setup_system ();
  void
  assemble_system (bool residual_only=false);
  void
  assemble_nl_residual ();

  void assemble_diag_mass_matrix();

  void
  set_initial_bc (
    const double time);
  void
  set_newton_bc ();

  unsigned int
  solve ();

  double newton_active_set();

  double
  newton_iteration (
    const double time);

  double
  compute_point_value (
    const DoFHandler<dim> &dofh, const LA::MPI::BlockVector &vector,
    const Point<dim> &p, const unsigned int component) const;

  void
  output_results () const;

  void
  compute_functional_values ();

  void
  compute_load();

  void compute_cod_array ();

  double
  compute_cod (
    const double eval_line);

  double compute_energy();

  bool
  refine_mesh ();
  void
  project_back_phase_field ();

  MPI_Comm mpi_com;

  const unsigned int degree;
  ParameterHandler &prm;

  parallel::distributed::Triangulation<dim> triangulation;

  FESystem<dim> fe;
  DoFHandler<dim> dof_handler;
  ConstraintMatrix constraints_update;
  ConstraintMatrix constraints_hanging_nodes;

  LA::MPI::BlockSparseMatrix system_pde_matrix;
  LA::MPI::BlockVector solution, newton_update,
  old_solution, old_old_solution, system_pde_residual;
  LA::MPI::BlockVector system_total_residual;

  LA::MPI::BlockVector diag_mass, diag_mass_relevant;

  ConditionalOStream pcout;
  TimerOutput timer;

  IndexSet active_set;

  Function<dim> *func_emodulus;

  std::vector<IndexSet> partition;
  std::vector<IndexSet> partition_relevant;

  std::vector<std::vector<bool> > constant_modes;

  LA::MPI::PreconditionAMG preconditioner_solid;
  LA::MPI::PreconditionAMG preconditioner_phase_field;

  // Global variables for timestepping scheme
  unsigned int timestep_number;
  unsigned int max_no_timesteps;
  double timestep, timestep_size_2, time;
  unsigned int switch_timestep;
  struct OuterSolverType
  {
    enum Enum {active_set, simple_monolithic};
  };
  typename OuterSolverType::Enum outer_solver;

  struct TestCase
  {
    enum Enum {sneddon_2d, miehe_tension, miehe_shear, multiple_homo, multiple_het};
  };
  typename TestCase::Enum test_case;

  struct RefinementStrategy
  {
    enum Enum {phase_field_ref, fixed_preref_sneddon, fixed_preref_miehe_tension,
               fixed_preref_miehe_shear, fixed_preref_multiple_homo, fixed_preref_multiple_het,
               global, mix
              };
  };
  typename RefinementStrategy::Enum refinement_strategy;

  bool direct_solver;

  double force_structure_x_biot, force_structure_y_biot;
  double force_structure_x, force_structure_y;

  // Biot parameters
  double c_biot, alpha_biot, lame_coefficient_biot, K_biot, density_biot;

  double gravity_x, gravity_y, volume_source, traction_x, traction_y,
         traction_x_biot, traction_y_biot;

  // Structure parameters
  double density_structure;
  double lame_coefficient_mu, lame_coefficient_lambda, poisson_ratio_nu;

  // Other parameters to control the fluid mesh motion
  double cell_diameter;

  FunctionParser<1> func_pressure;
  double constant_k, alpha_eps,
         G_c, viscosity_biot, gamma_penal;

  double E_modulus, E_prime;
  double min_cell_diameter, norm_part_iterations, value_phase_field_for_refinement;

  unsigned int n_global_pre_refine, n_local_pre_refine, n_refinement_cycles;

  double lower_bound_newton_residuum;
  unsigned int max_no_newton_steps;
  double upper_newton_rho;
  unsigned int max_no_line_search_steps;
  double line_search_damping;
  double decompose_stress_rhs, decompose_stress_matrix;
  std::string filename_basis;
  double old_timestep, old_old_timestep;
  bool use_old_timestep_pf;


};

// The constructor of this class is comparable
// to other tutorials steps, e.g., step-22, and step-31.
template <int dim>
FracturePhaseFieldProblem<dim>::FracturePhaseFieldProblem (
  const unsigned int degree, ParameterHandler &param)
  :
  mpi_com(MPI_COMM_WORLD),
  degree(degree),
  prm(param),
  triangulation(mpi_com),

  fe(FE_Q<dim>(degree), dim, FE_Q<dim>(degree), 1),
  dof_handler(triangulation),

  pcout(std::cout, (Utilities::MPI::this_mpi_process(mpi_com) == 0)),
  timer(mpi_com, pcout, TimerOutput::every_call_and_summary,
        TimerOutput::cpu_and_wall_times)
{
}



template <int dim>
void
FracturePhaseFieldProblem<dim>::declare_parameters (ParameterHandler &prm)
{
  prm.enter_subsection("Global parameters");
  {
    prm.declare_entry("Global pre-refinement steps", "1",
                      Patterns::Integer(0));

    prm.declare_entry("Local pre-refinement steps", "0",
                      Patterns::Integer(0));

    prm.declare_entry("Adaptive refinement cycles", "0",
                      Patterns::Integer(0));

    prm.declare_entry("Max No of timesteps", "1", Patterns::Integer(0));

    prm.declare_entry("Timestep size", "1.0", Patterns::Double(0));

    prm.declare_entry("Timestep size to switch to", "1.0", Patterns::Double(0));

    prm.declare_entry("Switch timestep after steps", "0", Patterns::Integer(0));

    prm.declare_entry("outer solver", "active set",
                      Patterns::Selection("active set|simple monolithic"));

    prm.declare_entry("test case", "sneddon 2d", Patterns::Selection("sneddon 2d|miehe tension|miehe shear|multiple homo|multiple het"));

    prm.declare_entry("ref strategy", "phase field",
                      Patterns::Selection("phase field|fixed preref sneddon|fixed preref miehe tension|fixed preref miehe shear|fixed preref multiple homo|fixed preref multiple het|global|mix"));

    prm.declare_entry("value phase field for refinement", "0.0", Patterns::Double(0));

    prm.declare_entry("Output filename", "solution_",
                      Patterns::Anything());
  }
  prm.leave_subsection();

  prm.enter_subsection("Problem dependent parameters");
  {
    prm.declare_entry("K reg", "1.0 * h", Patterns::Anything());

    prm.declare_entry("Eps reg", "1.0 * h", Patterns::Anything());

    prm.declare_entry("Gamma penalization", "0.0", Patterns::Double(0));

    prm.declare_entry("alpha time", "0.0", Patterns::Double(0));

    prm.declare_entry("Pressure", "0.0", Patterns::Anything());

    prm.declare_entry("Fracture toughness G_c", "0.0", Patterns::Double(0));

    prm.declare_entry("Density solid", "0.0", Patterns::Double(0));

    prm.declare_entry("Poisson ratio nu", "0.0", Patterns::Double(0));

    prm.declare_entry("E modulus", "0.0", Patterns::Double(0));

    prm.declare_entry("Lame mu", "0.0", Patterns::Double(0));

    prm.declare_entry("Lame lambda", "0.0", Patterns::Double(0));

  }
  prm.leave_subsection();

  prm.enter_subsection("Solver parameters");
  {
    prm.declare_entry("Use Direct Inner Solver", "false",
                      Patterns::Bool());

    prm.declare_entry("Newton lower bound", "1.0e-10",
                      Patterns::Double(0));

    prm.declare_entry("Newton maximum steps", "10",
                      Patterns::Integer(0));

    prm.declare_entry("Upper Newton rho", "0.999",
                      Patterns::Double(0));

    prm.declare_entry("Line search maximum steps", "5",
                      Patterns::Integer(0));

    prm.declare_entry("Line search damping", "0.5",
                      Patterns::Double(0));

    prm.declare_entry("Decompose stress in rhs", "0.0",
                      Patterns::Double(0));

    prm.declare_entry("Decompose stress in matrix", "0.0",
                      Patterns::Double(0));

  }
  prm.leave_subsection();

}



// In this method, we set up runtime parameters that
// could also come from a paramter file.
template <int dim>
void
FracturePhaseFieldProblem<dim>::set_runtime_parameters ()
{
  // Get parameters from file
  prm.enter_subsection("Global parameters");
  n_global_pre_refine = prm.get_integer("Global pre-refinement steps");
  n_local_pre_refine = prm.get_integer("Local pre-refinement steps");
  n_refinement_cycles = prm.get_integer("Adaptive refinement cycles");
  max_no_timesteps = prm.get_integer("Max No of timesteps");
  timestep = prm.get_double("Timestep size");
  timestep_size_2 = prm.get_double("Timestep size to switch to");
  switch_timestep = prm.get_integer("Switch timestep after steps");

  if (prm.get("outer solver")=="active set")
    outer_solver = OuterSolverType::active_set;
  else if (prm.get("outer solver")=="simple monolithic")
    outer_solver = OuterSolverType::simple_monolithic;

  if (prm.get("test case")=="sneddon 2d")
    test_case = TestCase::sneddon_2d;
  else if (prm.get("test case")=="miehe tension")
    test_case = TestCase::miehe_tension; // straight crack
  else if (prm.get("test case")=="miehe shear")
    test_case = TestCase::miehe_shear; // curved crack
  else if (prm.get("test case")=="multiple homo")
    test_case = TestCase::multiple_homo; // multiple fractures homogeneous material
  else if (prm.get("test case")=="multiple het")
    test_case = TestCase::multiple_het; // multiple fractures heterogeneous material
  else
    AssertThrow(false, ExcNotImplemented());

  if (prm.get("ref strategy")=="phase field")
    refinement_strategy = RefinementStrategy::phase_field_ref;
  else if (prm.get("ref strategy")=="fixed preref sneddon")
    refinement_strategy = RefinementStrategy::fixed_preref_sneddon;
  else if (prm.get("ref strategy")=="fixed preref miehe tension")
    refinement_strategy = RefinementStrategy::fixed_preref_miehe_tension;
  else if (prm.get("ref strategy")=="fixed preref miehe shear")
    refinement_strategy = RefinementStrategy::fixed_preref_miehe_shear;
  else if (prm.get("ref strategy")=="fixed preref multiple homo")
    refinement_strategy = RefinementStrategy::fixed_preref_multiple_homo;
  else if (prm.get("ref strategy")=="fixed preref multiple het")
    refinement_strategy = RefinementStrategy::fixed_preref_multiple_het;
  else if (prm.get("ref strategy")=="global")
    refinement_strategy = RefinementStrategy::global;
  else if (prm.get("ref strategy")=="mix")
    refinement_strategy = RefinementStrategy::mix;
  else
    AssertThrow(false, ExcNotImplemented());

  value_phase_field_for_refinement
    = prm.get_double("value phase field for refinement");

  filename_basis  = prm.get ("Output filename");

  prm.leave_subsection();

  prm.enter_subsection("Problem dependent parameters");

  // Phase-field parameters
  // They are given some values below
  constant_k = 0;//prm.get_double("K reg");
  alpha_eps = 0;//prm.get_double("Eps reg");

  // Switch between active set strategy
  // and simple penalization
  // in order to enforce crack irreversiblity
  if (outer_solver == OuterSolverType::active_set)
    gamma_penal = 0.0;
  else
    gamma_penal = prm.get_double("Gamma penalization");

  // Material and problem-rhs parameters
  func_pressure.initialize ("time", prm.get("Pressure"),
                            FunctionParser<1>::ConstMap());

  G_c = prm.get_double("Fracture toughness G_c");
  density_structure = prm.get_double("Density solid");

  // In all examples chosen as 0. Will be non-zero
  // if a Darcy fluid is computed
  alpha_biot = 0.0;


  if (test_case == TestCase::sneddon_2d ||
      test_case == TestCase::multiple_homo ||
      test_case == TestCase::multiple_het)
    {
      poisson_ratio_nu = prm.get_double("Poisson ratio nu");
      E_modulus = prm.get_double("E modulus");

      lame_coefficient_mu = E_modulus / (2.0 * (1 + poisson_ratio_nu));

      lame_coefficient_lambda = (2 * poisson_ratio_nu * lame_coefficient_mu)
                                / (1.0 - 2 * poisson_ratio_nu);
    }
  else
    {
      // Miehe 2010
      lame_coefficient_mu = prm.get_double("Lame mu");
      lame_coefficient_lambda = prm.get_double("Lame lambda");

      // Dummy
      poisson_ratio_nu = prm.get_double("Poisson ratio nu");
      E_modulus = prm.get_double("E modulus");
    }

  prm.leave_subsection();

  E_prime = E_modulus / (1.0 - poisson_ratio_nu * poisson_ratio_nu);

  // A variable to count the number of time steps
  timestep_number = 0;

  // Counts total time
  time = 0;

  // In the following, we read a *.inp grid from a file.
  // The configuration is based on Sneddon's benchmark (1969)
  // and Miehe 2010 (tension and shear)
  std::string grid_name;
  if (test_case == TestCase::sneddon_2d ||
      test_case == TestCase::multiple_homo ||
      test_case == TestCase::multiple_het)
    grid_name = "meshes/unit_square_4.inp";
  else
    grid_name  = "meshes/unit_slit.inp";

  GridIn<dim> grid_in;
  grid_in.attach_triangulation(triangulation);
  std::ifstream input_file(grid_name.c_str());
  Assert(dim==2, ExcInternalError());
  grid_in.read_ucd(input_file);

  triangulation.refine_global(n_global_pre_refine);

  pcout << "Cells:\t" << triangulation.n_active_cells() << std::endl;


  if (test_case == TestCase::multiple_het)
    func_emodulus = new BitmapFunction<dim>("test.pgm",0,4,0,4,E_modulus,10.0*E_modulus);



  prm.enter_subsection("Solver parameters");
  direct_solver = prm.get_bool("Use Direct Inner Solver");

  // Newton tolerances and maximum steps
  lower_bound_newton_residuum = prm.get_double("Newton lower bound");
  max_no_newton_steps = prm.get_integer("Newton maximum steps");


  // Criterion when time step should be cut
  // Higher number means: almost never
  // only used for simple penalization
  upper_newton_rho = prm.get_double("Upper Newton rho");

  // Line search control
  max_no_line_search_steps = prm.get_integer("Line search maximum steps");
  line_search_damping = prm.get_double("Line search damping");

  // Decompose stress in plus (tensile) and minus (compression)
  // 0.0: no decomposition, 1.0: with decomposition
  // Motivation see Miehe et al. (2010)
  decompose_stress_rhs = prm.get_double("Decompose stress in rhs");
  decompose_stress_matrix = prm.get_double("Decompose stress in matrix");

  // For pf_extra
  use_old_timestep_pf = false;

  prm.leave_subsection();
}


// This function is similar to many deal.II tuturial steps.
template <int dim>
void
FracturePhaseFieldProblem<dim>::setup_system ()
{
  // We set runtime parameters to drive the problem.
  // These parameters could also be read from a parameter file that
  // can be handled by the ParameterHandler object (see step-19)
  system_pde_matrix.clear();

  dof_handler.distribute_dofs(fe);

  std::vector<unsigned int> sub_blocks (dim+1,0);
  sub_blocks[dim] = 1;
  DoFRenumbering::component_wise (dof_handler, sub_blocks);

  FEValuesExtractors::Vector extract_displacement(0);
  constant_modes.clear();
  DoFTools::extract_constant_modes(dof_handler,
                                   fe.component_mask(extract_displacement), constant_modes);

  std::vector<types::global_dof_index> dofs_per_block (2);
  DoFTools::count_dofs_per_block (dof_handler, dofs_per_block, sub_blocks);
  const unsigned int n_solid = dofs_per_block[0];
  const unsigned int n_phase = dofs_per_block[1];
  pcout << std::endl;
  pcout << "DoFs: " << n_solid << " solid + " << n_phase << " phase = "
        << n_solid + n_phase << std::endl;

  partition.clear();
  if (direct_solver)
    {
      partition.push_back(dof_handler.locally_owned_dofs());
    }
  else
    {
      partition.push_back(dof_handler.locally_owned_dofs().get_view(0,n_solid));
      partition.push_back(dof_handler.locally_owned_dofs().get_view(n_solid,n_solid+n_phase));
    }

  IndexSet relevant_set;
  DoFTools::extract_locally_relevant_dofs(dof_handler, relevant_set);
  partition_relevant.clear();
  if (direct_solver)
    {
      partition_relevant.push_back(relevant_set);
    }
  else
    {
      partition_relevant.push_back(relevant_set.get_view(0,n_solid));
      partition_relevant.push_back(relevant_set.get_view(n_solid,n_solid+n_phase));
    }

  {
    constraints_hanging_nodes.clear();
    constraints_hanging_nodes.reinit(relevant_set);
    DoFTools::make_hanging_node_constraints(dof_handler,
                                            constraints_hanging_nodes);
    constraints_hanging_nodes.close();
  }
  {
    constraints_update.clear();
    constraints_update.reinit(relevant_set);

    set_newton_bc();
    constraints_update.merge(constraints_hanging_nodes);
    constraints_update.close();
  }

  {
    TrilinosWrappers::BlockSparsityPattern csp(partition, mpi_com);

    DoFTools::make_sparsity_pattern(dof_handler, csp,
                                    constraints_update,
                                    false,
                                    Utilities::MPI::this_mpi_process(mpi_com));

    csp.compress();
    system_pde_matrix.reinit(csp);
  }

  // Actual solution at time step n
  solution.reinit(partition);

  // Old timestep solution at time step n-1
  old_solution.reinit(partition_relevant);

  // Old timestep solution at time step n-2
  old_old_solution.reinit(partition_relevant);

  // Updates for Newton's method
  newton_update.reinit(partition);

  // Residual for  Newton's method
  system_pde_residual.reinit(partition);

  system_total_residual.reinit(partition);

  diag_mass.reinit(partition);
  diag_mass_relevant.reinit(partition_relevant);
  assemble_diag_mass_matrix();

  active_set.clear();
  active_set.set_size(dof_handler.n_dofs());

}


// Now, there follow several functions to perform
// the spectral decomposition of the stress tensor
// into tension and compression parts
// assumes the matrix is symmetric!
// The explicit calculation does only work
// in 2d. For 3d, we should use other libraries or approximative
// tools to compute eigenvectors and -functions.
// Borden et al. (2012, 2013) suggested some papers to look into.
template <int dim>
void eigen_vectors_and_values(
  double &E_eigenvalue_1, double &E_eigenvalue_2,
  Tensor<2,dim> &ev_matrix,
  const Tensor<2,dim> &matrix)
{
  double sq = std::sqrt((matrix[0][0] - matrix[1][1]) * (matrix[0][0] - matrix[1][1]) + 4.0*matrix[0][1]*matrix[1][0]);
  E_eigenvalue_1 = 0.5 * ((matrix[0][0] + matrix[1][1]) + sq);
  E_eigenvalue_2 = 0.5 * ((matrix[0][0] + matrix[1][1]) - sq);

  // Compute eigenvectors
  Tensor<1,dim> E_eigenvector_1;
  Tensor<1,dim> E_eigenvector_2;
  if (std::abs(matrix[0][1]) < 1e-10*std::abs(matrix[0][0]) 
          || std::abs(matrix[0][1]) < 1e-10*std::abs(matrix[1][1]))
    {
      // E is close to diagonal
      E_eigenvector_1[0]=0;
      E_eigenvector_1[1]=1;
      E_eigenvector_2[0]=1;
      E_eigenvector_2[1]=0;
    }
  else
    {
      E_eigenvector_1[0] = 1.0/(std::sqrt(1 + (E_eigenvalue_1 - matrix[0][0])/matrix[0][1] * (E_eigenvalue_1 - matrix[0][0])/matrix[0][1]));
      E_eigenvector_1[1] = (E_eigenvalue_1 - matrix[0][0])/(matrix[0][1] * (std::sqrt(1 + (E_eigenvalue_1 - matrix[0][0])/matrix[0][1] * (E_eigenvalue_1 - matrix[0][0])/matrix[0][1])));
      E_eigenvector_2[0] = 1.0/(std::sqrt(1 + (E_eigenvalue_2 - matrix[0][0])/matrix[0][1] * (E_eigenvalue_2 - matrix[0][0])/matrix[0][1]));
      E_eigenvector_2[1] = (E_eigenvalue_2 - matrix[0][0])/(matrix[0][1] * (std::sqrt(1 + (E_eigenvalue_2 - matrix[0][0])/matrix[0][1] * (E_eigenvalue_2 - matrix[0][0])/matrix[0][1])));
    }

  ev_matrix[0][0] = E_eigenvector_1[0];
  ev_matrix[0][1] = E_eigenvector_2[0];
  ev_matrix[1][0] = E_eigenvector_1[1];
  ev_matrix[1][1] = E_eigenvector_2[1];

  // Sanity check if orthogonal
  double scalar_prod = 1.0e+10;
  scalar_prod = E_eigenvector_1[0] * E_eigenvector_2[0] + E_eigenvector_1[1] * E_eigenvector_2[1];

  if (scalar_prod > 1.0e-6)
    {
      std::cout << "Seems not to be orthogonal" << std::endl;
      abort();
    }
}

template <int dim>
void decompose_stress(
  Tensor<2,dim> &stress_term_plus,
  Tensor<2,dim> &stress_term_minus,
  const Tensor<2, dim> &E,
  const double tr_E,
  const Tensor<2, dim> &E_LinU,
  const double tr_E_LinU,
  const double lame_coefficient_lambda,
  const double lame_coefficient_mu,
  const bool derivative)
{
  static const Tensor<2, dim> Identity =
    Tensors::get_Identity<dim>();

  Tensor<2, dim> zero_matrix;
  zero_matrix.clear();


  // Compute first the eigenvalues for u (as in the previous function)
  // and then for \delta u

  // Compute eigenvalues/vectors
  double E_eigenvalue_1, E_eigenvalue_2;
  Tensor<2,dim> P_matrix;
  eigen_vectors_and_values(E_eigenvalue_1, E_eigenvalue_2,P_matrix,E);

  double E_eigenvalue_1_plus = std::max(0.0, E_eigenvalue_1);
  double E_eigenvalue_2_plus = std::max(0.0, E_eigenvalue_2);

  Tensor<2,dim> Lambda_plus;
  Lambda_plus[0][0] = E_eigenvalue_1_plus;
  Lambda_plus[0][1] = 0.0;
  Lambda_plus[1][0] = 0.0;
  Lambda_plus[1][1] = E_eigenvalue_2_plus;

  if (!derivative)
    {
      Tensor<2,dim> E_plus = P_matrix * Lambda_plus * transpose(P_matrix);

      double tr_E_positive = std::max(0.0, tr_E);

      stress_term_plus = lame_coefficient_lambda * tr_E_positive * Identity
                         + 2 * lame_coefficient_mu * E_plus;

      stress_term_minus = lame_coefficient_lambda * (tr_E - tr_E_positive) * Identity
                          + 2 * lame_coefficient_mu * (E - E_plus);
    }
  else
    {
      // Derviatives (\delta u)

      // Compute eigenvalues/vectors
      double E_eigenvalue_1_LinU, E_eigenvalue_2_LinU;
      Tensor<1,dim> E_eigenvector_1_LinU;
      Tensor<1,dim> E_eigenvector_2_LinU;
      Tensor<2,dim> P_matrix_LinU;

      // Compute linearized Eigenvalues
      double diskriminante = std::sqrt(E[0][1] * E[1][0] + (E[0][0] - E[1][1]) * (E[0][0] - E[1][1])/4.0);

      E_eigenvalue_1_LinU = 0.5 * tr_E_LinU + 1.0/(2.0 * diskriminante) *
                            (E_LinU[0][1] * E[1][0] + E[0][1] * E_LinU[1][0] + (E[0][0] - E[1][1])*(E_LinU[0][0] - E_LinU[1][1])/2.0);

      E_eigenvalue_2_LinU = 0.5 * tr_E_LinU - 1.0/(2.0 * diskriminante) *
                            (E_LinU[0][1] * E[1][0] + E[0][1] * E_LinU[1][0] + (E[0][0] - E[1][1])*(E_LinU[0][0] - E_LinU[1][1])/2.0);


      // Compute normalized Eigenvectors and P
      double normalization_1 = 1.0/(std::sqrt(1 + (E_eigenvalue_1 - E[0][0])/E[0][1] * (E_eigenvalue_1 - E[0][0])/E[0][1]));
      double normalization_2 = 1.0/(std::sqrt(1 + (E_eigenvalue_2 - E[0][0])/E[0][1] * (E_eigenvalue_2 - E[0][0])/E[0][1]));

      double normalization_1_LinU = 0.0;
      double normalization_2_LinU = 0.0;

      normalization_1_LinU = -1.0 * (1.0/(1.0 + (E_eigenvalue_1 - E[0][0])/E[0][1] * (E_eigenvalue_1 - E[0][0])/E[0][1])
                                     * 1.0/(2.0 * std::sqrt(1.0 + (E_eigenvalue_1 - E[0][0])/E[0][1] * (E_eigenvalue_1 - E[0][0])/E[0][1]))
                                     * (2.0 * (E_eigenvalue_1 - E[0][0])/E[0][1])
                                     * ((E_eigenvalue_1_LinU - E_LinU[0][0]) * E[0][1] - (E_eigenvalue_1 - E[0][0]) * E_LinU[0][1])/(E[0][1] * E[0][1]));

      normalization_2_LinU = -1.0 * (1.0/(1.0 + (E_eigenvalue_2 - E[0][0])/E[0][1] * (E_eigenvalue_2 - E[0][0])/E[0][1])
                                     * 1.0/(2.0 * std::sqrt(1.0 + (E_eigenvalue_2 - E[0][0])/E[0][1] * (E_eigenvalue_2 - E[0][0])/E[0][1]))
                                     * (2.0 * (E_eigenvalue_2 - E[0][0])/E[0][1])
                                     * ((E_eigenvalue_2_LinU - E_LinU[0][0]) * E[0][1] - (E_eigenvalue_2 - E[0][0]) * E_LinU[0][1])/(E[0][1] * E[0][1]));


      E_eigenvector_1_LinU[0] = normalization_1 * 1.0;
      E_eigenvector_1_LinU[1] = normalization_1 * (E_eigenvalue_1 - E[0][0])/E[0][1];

      E_eigenvector_2_LinU[0] = normalization_2 * 1.0;
      E_eigenvector_2_LinU[1] = normalization_2 * (E_eigenvalue_2 - E[0][0])/E[0][1];


      // Apply product rule to normalization and vector entries
      double EV_1_part_1_comp_1 = 0.0;  // LinU in vector entries, normalization U
      double EV_1_part_1_comp_2 = 0.0;  // LinU in vector entries, normalization U
      double EV_1_part_2_comp_1 = 0.0;  // vector entries U, normalization LinU
      double EV_1_part_2_comp_2 = 0.0;  // vector entries U, normalization LinU

      double EV_2_part_1_comp_1 = 0.0;  // LinU in vector entries, normalization U
      double EV_2_part_1_comp_2 = 0.0;  // LinU in vector entries, normalization U
      double EV_2_part_2_comp_1 = 0.0;  // vector entries U, normalization LinU
      double EV_2_part_2_comp_2 = 0.0;  // vector entries U, normalization LinU

      // Effizienter spaeter, aber erst einmal uebersichtlich und verstehen!
      EV_1_part_1_comp_1 = normalization_1 * 0.0;
      EV_1_part_1_comp_2 = normalization_1 *
                           ((E_eigenvalue_1_LinU - E_LinU[0][0]) * E[0][1] - (E_eigenvalue_1 - E[0][0]) * E_LinU[0][1])/(E[0][1] * E[0][1]);

      EV_1_part_2_comp_1 = normalization_1_LinU * 1.0;
      EV_1_part_2_comp_2 = normalization_1_LinU * (E_eigenvalue_1 - E[0][0])/E[0][1];


      EV_2_part_1_comp_1 = normalization_2 * 0.0;
      EV_2_part_1_comp_2 = normalization_2 *
                           ((E_eigenvalue_2_LinU - E_LinU[0][0]) * E[0][1] - (E_eigenvalue_2 - E[0][0]) * E_LinU[0][1])/(E[0][1] * E[0][1]);

      EV_2_part_2_comp_1 = normalization_2_LinU * 1.0;
      EV_2_part_2_comp_2 = normalization_2_LinU * (E_eigenvalue_2 - E[0][0])/E[0][1];



      // Build eigenvectors
      E_eigenvector_1_LinU[0] = EV_1_part_1_comp_1 + EV_1_part_2_comp_1;
      E_eigenvector_1_LinU[1] = EV_1_part_1_comp_2 + EV_1_part_2_comp_2;

      E_eigenvector_2_LinU[0] = EV_2_part_1_comp_1 + EV_2_part_2_comp_1;
      E_eigenvector_2_LinU[1] = EV_2_part_1_comp_2 + EV_2_part_2_comp_2;



      // P-Matrix
      P_matrix_LinU[0][0] = E_eigenvector_1_LinU[0];
      P_matrix_LinU[0][1] = E_eigenvector_2_LinU[0];
      P_matrix_LinU[1][0] = E_eigenvector_1_LinU[1];
      P_matrix_LinU[1][1] = E_eigenvector_2_LinU[1];


      double E_eigenvalue_1_plus_LinU = 0.0;
      double E_eigenvalue_2_plus_LinU = 0.0;


      // Very important: Set E_eigenvalue_1_plus_LinU to zero when
      // the corresponding rhs-value is set to zero and NOT when
      // the value itself is negative!!!
      if (E_eigenvalue_1 < 0.0)
        {
          E_eigenvalue_1_plus_LinU = 0.0;
        }
      else
        E_eigenvalue_1_plus_LinU = E_eigenvalue_1_LinU;


      if (E_eigenvalue_2 < 0.0)
        {
          E_eigenvalue_2_plus_LinU = 0.0;
        }
      else
        E_eigenvalue_2_plus_LinU = E_eigenvalue_2_LinU;



      Tensor<2,dim> Lambda_plus_LinU;
      Lambda_plus_LinU[0][0] = E_eigenvalue_1_plus_LinU;
      Lambda_plus_LinU[0][1] = 0.0;
      Lambda_plus_LinU[1][0] = 0.0;
      Lambda_plus_LinU[1][1] = E_eigenvalue_2_plus_LinU;

      Tensor<2,dim> E_plus_LinU = P_matrix_LinU * Lambda_plus * transpose(P_matrix) +  P_matrix * Lambda_plus_LinU * transpose(P_matrix) + P_matrix * Lambda_plus * transpose(P_matrix_LinU);


      double tr_E_positive_LinU = 0.0;
      if (tr_E < 0.0)
        {
          tr_E_positive_LinU = 0.0;

        }
      else
        tr_E_positive_LinU = tr_E_LinU;



      stress_term_plus = lame_coefficient_lambda * tr_E_positive_LinU * Identity
                         + 2 * lame_coefficient_mu * E_plus_LinU;

      stress_term_minus = lame_coefficient_lambda * (tr_E_LinU - tr_E_positive_LinU) * Identity
                          + 2 * lame_coefficient_mu * (E_LinU - E_plus_LinU);


      // Sanity check
      //Tensor<2,dim> stress_term = lame_coefficient_lambda * tr_E_LinU * Identity
      //  + 2 * lame_coefficient_mu * E_LinU;

      //std::cout << stress_term.norm() << "   " << stress_term_plus.norm() << "   " << stress_term_minus.norm() << std::endl;
    }


}






// In this function, we assemble the Jacobian matrix
// for the Newton iteration.
template <int dim>
void
FracturePhaseFieldProblem<dim>::assemble_system (bool residual_only)
{
  if (residual_only)
    system_total_residual = 0;
  else
    system_pde_matrix = 0;
  system_pde_residual = 0;

  // This function is only necessary
  // when working with simple penalization
  if ((outer_solver == OuterSolverType::simple_monolithic) && (timestep_number < 1))
    {
      gamma_penal = 0.0;
    }
  const double current_pressure = func_pressure.value(Point<1>(time), 0);

  LA::MPI::BlockVector rel_solution(
    partition_relevant);
  rel_solution = solution;

  LA::MPI::BlockVector rel_old_solution(
    partition_relevant);
  rel_old_solution = old_solution;

  LA::MPI::BlockVector rel_old_old_solution(
    partition_relevant);
  rel_old_old_solution = old_old_solution;

  QGauss<dim> quadrature_formula(degree + 2);

  FEValues<dim> fe_values(fe, quadrature_formula,
                          update_values | update_quadrature_points | update_JxW_values
                          | update_gradients);

  const unsigned int dofs_per_cell = fe.dofs_per_cell;

  const unsigned int n_q_points = quadrature_formula.size();

  FullMatrix<double> local_matrix(dofs_per_cell, dofs_per_cell);
  Vector<double> local_rhs(dofs_per_cell);

  std::vector<unsigned int> local_dof_indices(dofs_per_cell);

  const FEValuesExtractors::Vector displacements(0);
  const FEValuesExtractors::Scalar phase_field (dim); // 2

  std::vector<Vector<double> > old_solution_values(n_q_points,
                                                   Vector<double>(dim+1));

  std::vector<std::vector<Tensor<1,dim> > > old_solution_grads (n_q_points,
      std::vector<Tensor<1,dim> > (dim+1));

  std::vector<Vector<double> > old_timestep_solution_values(n_q_points,
                                                            Vector<double>(dim+1));

  std::vector<std::vector<Tensor<1,dim> > > old_timestep_solution_grads (n_q_points,
      std::vector<Tensor<1,dim> > (dim+1));

  std::vector<Vector<double> > old_old_timestep_solution_values(n_q_points,
      Vector<double>(dim+1));

  // Declaring test functions:
  std::vector<Tensor<1, dim> > phi_i_u(dofs_per_cell);
  std::vector<Tensor<2, dim> > phi_i_grads_u(dofs_per_cell);
  std::vector<double>          phi_i_pf(dofs_per_cell);
  std::vector<Tensor<1,dim> >  phi_i_grads_pf (dofs_per_cell);

  typename DoFHandler<dim>::active_cell_iterator cell =
    dof_handler.begin_active(), endc = dof_handler.end();

  Tensor<2,dim> zero_matrix;
  zero_matrix.clear();

  for (; cell != endc; ++cell)
    if (cell->is_locally_owned())
      {
        fe_values.reinit(cell);

        // update lame coefficients based on current cell
        // when working with heterogeneous materials
        if (test_case == TestCase::multiple_het)
          {
            E_modulus = func_emodulus->value(cell->center(), 0);
            E_modulus += 1.0;

            lame_coefficient_mu = E_modulus / (2.0 * (1 + poisson_ratio_nu));

            lame_coefficient_lambda = (2 * poisson_ratio_nu * lame_coefficient_mu)
                                      / (1.0 - 2 * poisson_ratio_nu);
          }

        local_matrix = 0;
        local_rhs = 0;

        // Old Newton iteration values
        fe_values.get_function_values (rel_solution, old_solution_values);
        fe_values.get_function_gradients (rel_solution, old_solution_grads);

        // Old_timestep_solution values
        fe_values.get_function_values (rel_old_solution, old_timestep_solution_values);

        // Old Old_timestep_solution values
        fe_values.get_function_values (rel_old_old_solution, old_old_timestep_solution_values);

        {
          for (unsigned int q = 0; q < n_q_points; ++q)
            {
              for (unsigned int k = 0; k < dofs_per_cell; ++k)
                {
                  phi_i_u[k]       = fe_values[displacements].value(k, q);
                  phi_i_grads_u[k] = fe_values[displacements].gradient(k, q);
                  phi_i_pf[k]       = fe_values[phase_field].value (k, q);
                  phi_i_grads_pf[k] = fe_values[phase_field].gradient (k, q);

                }

              // First, we prepare things coming from the previous Newton
              // iteration...
              double pf = old_solution_values[q](dim);
              double old_timestep_pf = old_timestep_solution_values[q](dim);
              double old_old_timestep_pf = old_old_timestep_solution_values[q](dim);
              if (outer_solver == OuterSolverType::simple_monolithic)
                {
                  pf = std::max(0.0,old_solution_values[q](dim));
                  old_timestep_pf = std::max(0.0,old_timestep_solution_values[q](dim));
                  old_old_timestep_pf = std::max(0.0,old_old_timestep_solution_values[q](dim));
                }


              double pf_minus_old_timestep_pf_plus =
                std::max(0.0, pf - old_timestep_pf);

              double pf_extra = pf;
              // Linearization by extrapolation to cope with non-convexity of the underlying
              // energy functional.
              // This idea might be refined in a future work (be also careful because
              // theoretically, we do not have time regularity; therefore extrapolation in time
              // might be questionable. But for the time being, this is numerically robust.
              pf_extra = old_old_timestep_pf + (time - (time-old_timestep-old_old_timestep))/
                         (time-old_timestep - (time-old_timestep-old_old_timestep)) * (old_timestep_pf - old_old_timestep_pf);
              if (pf_extra <= 0.0)
                pf_extra = 0.0;
              if (pf_extra >= 1.0)
                pf_extra = 1.0;


              if (use_old_timestep_pf)
                pf_extra = old_timestep_pf;


              const Tensor<2,dim> grad_u = Tensors
                                           ::get_grad_u<dim> (q, old_solution_grads);

              const Tensor<1,dim> grad_pf = Tensors
                                            ::get_grad_pf<dim> (q, old_solution_grads);

              const double divergence_u = old_solution_grads[q][0][0] +
                                          old_solution_grads[q][1][1];

              const Tensor<2,dim> Identity = Tensors
                                             ::get_Identity<dim> ();

              const Tensor<2,dim> E = 0.5 * (grad_u + transpose(grad_u));
              const double tr_E = grad_u[0][0] + grad_u[1][1];

              Tensor<2,dim> stress_term_plus;
              Tensor<2,dim> stress_term_minus;
              if (decompose_stress_matrix>0 && timestep_number>0)
                {
                  decompose_stress(stress_term_plus, stress_term_minus,
                                   E, tr_E, zero_matrix , 0.0,
                                   lame_coefficient_lambda,
                                   lame_coefficient_mu, false);
                }
              else
                {
                  stress_term_plus = lame_coefficient_lambda * tr_E * Identity
                                     + 2 * lame_coefficient_mu * E;
                  stress_term_minus = 0;
                }

              if (!residual_only)
                for (unsigned int i = 0; i < dofs_per_cell; ++i)
                  {
                    double pf_minus_old_timestep_pf_plus = 0.0;
                    if ((pf - old_timestep_pf) < 0.0)
                      pf_minus_old_timestep_pf_plus = 0.0;
                    else
                      pf_minus_old_timestep_pf_plus = phi_i_pf[i];


                    const Tensor<2, dim> E_LinU = 0.5
                                                  * (phi_i_grads_u[i] + transpose(phi_i_grads_u[i]));
                    const double tr_E_LinU = trace(E_LinU);

                    Tensor<2,dim> stress_term_LinU;
                    stress_term_LinU = lame_coefficient_lambda * tr_E_LinU * Identity
                                       + 2 * lame_coefficient_mu * E_LinU;

                    Tensor<2,dim> stress_term_plus_LinU;
                    Tensor<2,dim> stress_term_minus_LinU;

                    const unsigned int comp_i = fe.system_to_component_index(i).first;
                    if (comp_i == dim)
                      {
                        stress_term_plus_LinU = 0;
                        stress_term_minus_LinU = 0;
                      }
                    else if (decompose_stress_matrix > 0.0 && timestep_number>0)
                      {
                        decompose_stress(stress_term_plus_LinU, stress_term_minus_LinU,
                                         E, tr_E, E_LinU, tr_E_LinU,
                                         lame_coefficient_lambda,
                                         lame_coefficient_mu,
                                         true);
                      }
                    else
                      {
                        stress_term_plus_LinU = lame_coefficient_lambda * tr_E_LinU * Identity
                                                + 2 * lame_coefficient_mu * E_LinU;
                        stress_term_minus = 0;
                      }

                    for (unsigned int j = 0; j < dofs_per_cell; ++j)
                      {
                        const unsigned int comp_j = fe.system_to_component_index(j).first;
                        if (comp_j < dim)
                          {
                            // Solid
                            local_matrix(j,i) += 1.0 *
                                                 (scalar_product(((1-constant_k) * pf_extra * pf_extra + constant_k) *
                                                                 stress_term_plus_LinU, phi_i_grads_u[j])
                                                  // stress term minus
                                                  + decompose_stress_matrix * scalar_product(stress_term_minus_LinU, phi_i_grads_u[j])
                                                 ) * fe_values.JxW(q);

                          }
                        else if (comp_j == dim)
                          {
                            // Simple penalization for simple monolithic
                            local_matrix(j,i) += gamma_penal/timestep * 1.0/(cell->diameter() * cell->diameter()) *
                                                 pf_minus_old_timestep_pf_plus * phi_i_pf[j] * fe_values.JxW(q);

                            // Phase-field
                            local_matrix(j,i) +=
                              ((1-constant_k) * (scalar_product(stress_term_plus_LinU, E)
                                                 + scalar_product(stress_term_plus, E_LinU)) * pf * phi_i_pf[j]
                               +(1-constant_k) * scalar_product(stress_term_plus, E) * phi_i_pf[i] * phi_i_pf[j]
                               + G_c/alpha_eps * phi_i_pf[i] * phi_i_pf[j]
                               + G_c * alpha_eps * phi_i_grads_pf[i] * phi_i_grads_pf[j]
                               // Pressure terms
                               - 2.0 * (alpha_biot - 1.0) * current_pressure *
                               (pf * (phi_i_grads_u[i][0][0] + phi_i_grads_u[i][1][1])
                                + phi_i_pf[i] * divergence_u) * phi_i_pf[j]
                              ) * fe_values.JxW(q);
                          }

                        // end j dofs
                      }
                    // end i dofs
                  }


              // RHS:
              for (unsigned int i = 0; i < dofs_per_cell; ++i)
                {
                  const unsigned int comp_i = fe.system_to_component_index(i).first;
                  if (comp_i < dim)
                    {
                      const Tensor<2, dim> phi_i_grads_u =
                        fe_values[displacements].gradient(i, q);

                      // Solid
                      local_rhs(i) -=
                        (scalar_product(((1.0-constant_k) * pf_extra * pf_extra + constant_k) *
                                        stress_term_plus, phi_i_grads_u)
                         +  decompose_stress_rhs * scalar_product(stress_term_minus, phi_i_grads_u)
                         // Pressure terms
                         - (alpha_biot - 1.0) * current_pressure * pf_extra * pf_extra * (phi_i_grads_u[0][0] + phi_i_grads_u[1][1])
                        ) * fe_values.JxW(q);

                    }
                  else if (comp_i == dim)
                    {
                      const double phi_i_pf = fe_values[phase_field].value (i, q);
                      const Tensor<1,dim> phi_i_grads_pf = fe_values[phase_field].gradient (i, q);

                      // Simple penalization
                      local_rhs(i) -= gamma_penal/timestep * 1.0/(cell->diameter() * cell->diameter()) *
                                      pf_minus_old_timestep_pf_plus * phi_i_pf * fe_values.JxW(q);

                      // Phase field
                      local_rhs(i) -=
                        ((1.0 - constant_k) * scalar_product(stress_term_plus, E) * pf * phi_i_pf
                         - G_c/alpha_eps * (1.0 - pf) * phi_i_pf
                         + G_c * alpha_eps * grad_pf * phi_i_grads_pf
                         // Pressure terms
                         - 2.0 * (alpha_biot - 1.0) * current_pressure * pf * divergence_u * phi_i_pf
                        ) * fe_values.JxW(q);
                    }

                } // end i



              // end n_q_points
            }

          cell->get_dof_indices(local_dof_indices);
          if (residual_only)
            {
              constraints_update.distribute_local_to_global(local_rhs,
                                                            local_dof_indices, system_pde_residual);


              if (outer_solver == OuterSolverType::active_set)
                {
                  constraints_hanging_nodes.distribute_local_to_global(local_rhs,
                                                                       local_dof_indices, system_total_residual);
                }
              else
                {
                  constraints_update.distribute_local_to_global(local_rhs,
                                                                local_dof_indices, system_total_residual);
                }
            }
          else
            {
              constraints_update.distribute_local_to_global(local_matrix,
                                                            local_rhs,
                                                            local_dof_indices,
                                                            system_pde_matrix,
                                                            system_pde_residual);
            }
          // end if (second PDE: STVK material)
        }
        // end cell
      }

  if (residual_only)
    system_total_residual.compress(VectorOperation::add);
  else
    system_pde_matrix.compress(VectorOperation::add);

  system_pde_residual.compress(VectorOperation::add);

  if (!direct_solver && !residual_only)
    {
      {
        LA::MPI::PreconditionAMG::AdditionalData data;
        data.constant_modes = constant_modes;
        data.elliptic = true;
        data.higher_order_elements = true;
        data.smoother_sweeps = 2;
        data.aggregation_threshold = 0.02;
        preconditioner_solid.initialize(system_pde_matrix.block(0, 0), data);
      }
      {
        LA::MPI::PreconditionAMG::AdditionalData data;
        //data.constant_modes = constant_modes;
        data.elliptic = true;
        data.higher_order_elements = true;
        data.smoother_sweeps = 2;
        data.aggregation_threshold = 0.02;
        preconditioner_phase_field.initialize(system_pde_matrix.block(1, 1), data);
      }
    }
}




// In this function we assemble the semi-linear
// of the right hand side of Newton's method (its residual).
// The framework is in principal the same as for the
// system matrix.
template <int dim>
void
FracturePhaseFieldProblem<dim>::assemble_nl_residual ()
{
  assemble_system(true);
}

template <int dim>
void
FracturePhaseFieldProblem<dim>::assemble_diag_mass_matrix ()
{
  diag_mass = 0;

  QGaussLobatto<dim> quadrature_formula(degree + 1);
  //QGauss<dim> quadrature_formula(degree + 2);

  FEValues<dim> fe_values(fe, quadrature_formula,
                          update_values | update_quadrature_points | update_JxW_values
                          | update_gradients);

  const unsigned int dofs_per_cell = fe.dofs_per_cell;

  const unsigned int n_q_points = quadrature_formula.size();

  Vector<double> local_rhs(dofs_per_cell);

  std::vector<unsigned int> local_dof_indices(dofs_per_cell);

  typename DoFHandler<dim>::active_cell_iterator cell =
    dof_handler.begin_active(), endc = dof_handler.end();

  for (; cell != endc; ++cell)
    if (cell->is_locally_owned())
      {
        fe_values.reinit(cell);
        local_rhs = 0;

        for (unsigned int q_point = 0; q_point < n_q_points; ++q_point)
          for (unsigned int i = 0; i < dofs_per_cell; ++i)
            {
              const unsigned int comp_i = fe.system_to_component_index(i).first;
              if (comp_i != dim)
                continue; // only look at phase field

              local_rhs (i) += fe_values.shape_value(i, q_point) *
                               fe_values.shape_value(i, q_point) * fe_values.JxW(q_point);
            }

        cell->get_dof_indices(local_dof_indices);
        for (unsigned int i=0; i<dofs_per_cell; i++)
          diag_mass(local_dof_indices[i]) += local_rhs(i);


      }

  diag_mass.compress(VectorOperation::add);
  diag_mass_relevant = diag_mass;
}



// Here, we impose boundary conditions
// for the system and the first Newton step
template <int dim>
void
FracturePhaseFieldProblem<dim>::set_initial_bc (
  const double time)
{
  std::map<unsigned int, double> boundary_values;
  std::vector<bool> component_mask(dim+1, false);
  if (test_case == TestCase::sneddon_2d ||
      test_case == TestCase::multiple_homo ||
      test_case == TestCase::multiple_het)
    {
      component_mask[0] = true;
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 0,
                                               ZeroFunction<dim>(dim+1), boundary_values, component_mask);

      component_mask[0] = true;
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 1,
                                               ZeroFunction<dim>(dim+1), boundary_values, component_mask);

      component_mask[0] = true;
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 2,
                                               ZeroFunction<dim>(dim+1), boundary_values, component_mask);

      component_mask[0] = true;
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 3,
                                               ZeroFunction<dim>(dim+1), boundary_values, component_mask);
    }
  else if (test_case == TestCase::miehe_tension)
    {
      // For Miehe 2010 tension test

      component_mask[0] = false;
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 2,
                                               ZeroFunction<dim>(dim+1), boundary_values, component_mask);

      component_mask[0] = true;
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 3,
                                               BoundaryParabelTension<dim>(time), boundary_values, component_mask);
    }
  else if (test_case == TestCase::miehe_shear)
    {
      // For Miehe 2010 shear test
      component_mask[0] = false;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 0,
                                               ZeroFunction<dim>(dim+1), boundary_values, component_mask);

      component_mask[0] = false;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 1,
                                               ZeroFunction<dim>(dim+1), boundary_values, component_mask);



      component_mask[0] = true;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 2,
                                               ZeroFunction<dim>(dim+1), boundary_values, component_mask);

      component_mask[0] = true;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 3,
                                               BoundaryParabelShear<dim>(time), boundary_values, component_mask);


      //      bottom part of crack
      component_mask[0] = false;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 4,
                                               ZeroFunction<dim>(dim+1), boundary_values, component_mask);

    }


  std::pair<unsigned int, unsigned int> range;

  if (direct_solver)
    {
      // this is not elegant, but works
      std::vector<unsigned int> sub_blocks (dim+1,0);
      sub_blocks[dim] = 1;
      std::vector<types::global_dof_index> dofs_per_block (2);
      DoFTools::count_dofs_per_block (dof_handler, dofs_per_block, sub_blocks);
      const unsigned int n_solid = dofs_per_block[0];
      IndexSet is = solution.block(0).locally_owned_elements().get_view(0, n_solid);
      Assert(is.is_contiguous(), ExcInternalError());
      range.first = is.nth_index_in_set(0);
      range.second = is.nth_index_in_set(is.n_elements()-1)+1;
    }
  else
    {
      range = solution.block(0).local_range();
    }
  for (typename std::map<unsigned int, double>::const_iterator i =
         boundary_values.begin(); i != boundary_values.end(); ++i)
    if (i->first >= range.first && i->first < range.second)
      solution(i->first) = i->second;

  solution.compress(VectorOperation::insert);

}

// This function applies boundary conditions
// to the Newton iteration steps. For all variables that
// have Dirichlet conditions on some (or all) parts
// of the outer boundary, we apply zero-Dirichlet
// conditions, now.
template <int dim>
void
FracturePhaseFieldProblem<dim>::set_newton_bc ()
{
  std::vector<bool> component_mask(dim+1, false);
  if (test_case == TestCase::sneddon_2d ||
      test_case == TestCase::multiple_homo ||
      test_case == TestCase::multiple_het)
    {
      component_mask[0] = true;
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 0,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);

      component_mask[0] = true;
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 1,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);



      component_mask[0] = true; // false
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 2,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);

      component_mask[0] = true; // false
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 3,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);
    }
  else if (test_case == TestCase::miehe_tension)
    {
      // Miehe 2010 tension
      component_mask[0] = false; // false
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 2,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);

      component_mask[0] = true; // false
      component_mask[1] = true;
      VectorTools::interpolate_boundary_values(dof_handler, 3,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);
    }
  else if (test_case == TestCase::miehe_shear)
    {
      // Miehe 2010 shear
      component_mask[0] = false;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 0,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);

      component_mask[0] = false;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 1,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);


      component_mask[0] = true;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 2,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);

      component_mask[0] = true;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 3,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);

      // bottom part of crack
      component_mask[0] = false;
      component_mask[1] = true;
      component_mask[2] = false;
      VectorTools::interpolate_boundary_values(dof_handler, 4,
                                               ZeroFunction<dim>(dim+1), constraints_update, component_mask);


    }


}


template <class PreconditionerA, class PreconditionerC>
class BlockDiagonalPreconditioner
{
public:
  BlockDiagonalPreconditioner(const LA::MPI::BlockSparseMatrix  &M,
                              const PreconditionerA &pre_A, const PreconditionerC &pre_C)
    : matrix(M),
      prec_A (pre_A),
      prec_C (pre_C)
  {
  }

  void vmult (LA::MPI::BlockVector       &dst,
              const LA::MPI::BlockVector &src) const
  {
    prec_A.vmult(dst.block(0), src.block(0));
    prec_C.vmult(dst.block(1), src.block(1));
  }


  const LA::MPI::BlockSparseMatrix &matrix;
  const PreconditionerA &prec_A;
  const PreconditionerC   &prec_C;
};

// In this function, we solve the linear systems
// inside the nonlinear Newton iteration.
template <int dim>
unsigned int
FracturePhaseFieldProblem<dim>::solve ()
{
  newton_update = 0;

  if (direct_solver)
    {
      SolverControl cn;
      TrilinosWrappers::SolverDirect solver(cn);
      solver.solve(system_pde_matrix.block(0,0), newton_update.block(0), system_pde_residual.block(0));

      constraints_update.distribute(newton_update);

      return 1;
    }
  else
    {
      SolverControl solver_control(200, system_pde_residual.l2_norm() * 1e-8);

      SolverGMRES<LA::MPI::BlockVector> solver(solver_control);

      BlockDiagonalPreconditioner<LA::MPI::PreconditionAMG,LA::MPI::PreconditionAMG>
      preconditioner(system_pde_matrix,
                     preconditioner_solid, preconditioner_phase_field);

      solver.solve(system_pde_matrix, newton_update,
                   system_pde_residual, preconditioner);

      constraints_update.distribute(newton_update);

      return solver_control.last_step();
    }
}


template <int dim>
double FracturePhaseFieldProblem<dim>::newton_active_set()
{
  pcout << "It.\t#A.Set\tResidual\tReduction\tLSrch\t#LinIts" << std::endl;

  LA::MPI::BlockVector residual_relevant(partition_relevant);

  set_initial_bc(time);
  constraints_hanging_nodes.distribute(solution);

  assemble_nl_residual();
  residual_relevant = system_total_residual;

  constraints_update.set_zero(system_pde_residual);
  double newton_residual = system_pde_residual.l2_norm();

  double old_newton_residual = newton_residual;
  unsigned int newton_step = 1;

  pcout << "0\t\t" << std::scientific << newton_residual << std::endl;
  std::cout.unsetf(std::ios_base::floatfield);

  active_set.clear();
  active_set.set_size(dof_handler.n_dofs());

  LA::MPI::BlockVector old_solution_relevant(partition_relevant);
  old_solution_relevant = old_solution;

  unsigned int it=0;

  double new_newton_residual = 0.0;
  while (true)
    {
      ++it;
      pcout << it << std::flush;

      IndexSet active_set_old = active_set;

      {
        // compute new active set
        active_set.clear();
        active_set.set_size(dof_handler.n_dofs());
        constraints_update.clear();
        unsigned int owned_active_set_dofs = 0;

        LA::MPI::BlockVector solution_relevant(partition_relevant);
        solution_relevant = solution;

        std::vector<unsigned int> local_dof_indices(fe.dofs_per_cell);
        typename DoFHandler<dim>::active_cell_iterator cell =
          dof_handler.begin_active(), endc = dof_handler.end();

        for (; cell != endc; ++cell)
          {
            if (! cell->is_locally_owned())
              continue;

            cell->get_dof_indices(local_dof_indices);
            for (unsigned int i=0; i<fe.dofs_per_cell; ++i)
              {
                const unsigned int comp_i = fe.system_to_component_index(i).first;
                if (comp_i != dim)
                  continue; // only look at phase field

                const unsigned int idx = local_dof_indices[i];

                double old_value = old_solution_relevant(idx);
                double new_value = solution_relevant(idx);

                //already processed or a hanging node?
                if (active_set.is_element(idx)
                    || constraints_hanging_nodes.is_constrained(idx))
                  continue;

                double c= 1e+1 * E_modulus;
                double massm = diag_mass_relevant(idx);

                double gap = new_value - old_value;

                if ( residual_relevant(idx)/massm + c * (gap) <= 0)
                  continue;

                // now idx is in the active set
                constraints_update.add_line(idx);
                constraints_update.set_inhomogeneity(idx, 0.0);
                solution(idx) = old_value;
                active_set.add_index(idx);

                if (dof_handler.locally_owned_dofs().is_element(idx))
                  ++owned_active_set_dofs;
              }
          }
        solution.compress(VectorOperation::insert);
        // we might have changed values of the solution, so fix the
        // hanging nodes (we ignore in the active set):
        constraints_hanging_nodes.distribute(solution);

        pcout << "\t"
              << Utilities::MPI::sum(owned_active_set_dofs, mpi_com)
              << std::flush;


      }

      set_newton_bc();
      constraints_update.merge(constraints_hanging_nodes);
      constraints_update.close();

      int is_my_set_changed = (active_set == active_set_old) ? 0 : 1;
      int num_changed = Utilities::MPI::sum(is_my_set_changed,
                                            MPI_COMM_WORLD);

      assemble_system();
      constraints_update.set_zero(system_pde_residual);
      unsigned int no_linear_iterations = solve();

      LA::MPI::BlockVector saved_solution = solution;

      if (false)
        {
          solution += newton_update;
          project_back_phase_field();
          //output_results();

          assemble_nl_residual();
          constraints_update.set_zero(system_pde_residual);
          pcout << "full step res: " << system_pde_residual.l2_norm() << " " << std::endl;
          solution = saved_solution;
          assemble_nl_residual();
          constraints_update.set_zero(system_pde_residual);
          pcout << "0-size res: " << system_pde_residual.l2_norm() << " " << std::endl;
        }

      // line search:
      unsigned int line_search_step = 0;

      for (; line_search_step < max_no_line_search_steps; ++line_search_step)
        {
          solution += newton_update;

          assemble_nl_residual();
          residual_relevant = system_total_residual;
          constraints_update.set_zero(system_pde_residual);
          new_newton_residual = system_pde_residual.l2_norm();


          if (new_newton_residual < newton_residual)
            break;

          solution = saved_solution;
          newton_update *= line_search_damping;
        }
      pcout << std::scientific
            << "\t" << new_newton_residual
            << "\t" << new_newton_residual/newton_residual;
      std::cout.unsetf(std::ios_base::floatfield);
      pcout << "\t" << line_search_step
            << "\t" << no_linear_iterations
            << std::endl;

      old_newton_residual = newton_residual;
      newton_residual = new_newton_residual;

      // Updates
      newton_step++;

      if (newton_residual < lower_bound_newton_residuum
          && num_changed == 0
         )
        {
          break;
        }

      if (it>=max_no_newton_steps)
        {
          pcout << "Newton iteration did not converge in " << it
                << " steps." << std::endl;
          throw SolverControl::NoConvergence(0,0);
        }


    }
  return new_newton_residual/old_newton_residual;

}


template <int dim>
double
FracturePhaseFieldProblem<dim>::newton_iteration (
  const double time)

{
  pcout << "It.\tResidual\tReduction\tLSrch\t\t#LinIts" << std::endl;

  // Decision whether the system matrix should be build
  // at each Newton step
  const double nonlinear_rho = 0.1;

  // Line search parameters
  unsigned int line_search_step = 0;
  double new_newton_residuum = 0.0;

  // Application of the initial boundary conditions to the
  // variational equations:
  set_initial_bc(time);
  assemble_nl_residual();
  constraints_update.set_zero(system_pde_residual);

  double newton_residuum = system_pde_residual.linfty_norm();
  double old_newton_residuum = newton_residuum;
  unsigned int newton_step = 1;
  unsigned int no_linear_iterations = 0;

  pcout << "0\t" << std::scientific << newton_residuum << std::endl;

  while (newton_residuum > lower_bound_newton_residuum
         && newton_step < max_no_newton_steps)
    {
      old_newton_residuum = newton_residuum;

      assemble_nl_residual();
      constraints_update.set_zero(system_pde_residual);
      newton_residuum = system_pde_residual.linfty_norm();

      if (newton_residuum < lower_bound_newton_residuum)
        {
          pcout << '\t' << std::scientific << newton_residuum << std::endl;
          break;
        }

      if (newton_step==1 || newton_residuum / old_newton_residuum > nonlinear_rho)
        assemble_system();

      // Solve Ax = b
      no_linear_iterations = solve();

      line_search_step = 0;
      for (; line_search_step < max_no_line_search_steps; ++line_search_step)
        {
          solution += newton_update;

          assemble_nl_residual();
          constraints_update.set_zero(system_pde_residual);
          new_newton_residuum = system_pde_residual.linfty_norm();

          if (new_newton_residuum < newton_residuum)
            break;
          else
            solution -= newton_update;

          newton_update *= line_search_damping;
        }
      old_newton_residuum = newton_residuum;
      newton_residuum = new_newton_residuum;

      pcout << std::setprecision(5) << newton_step << '\t' << std::scientific
            << newton_residuum;

      if (!direct_solver)
        pcout << " (" << system_pde_residual.block(0).linfty_norm() << '|'
              << system_pde_residual.block(1).linfty_norm() << ")";

      pcout << '\t' << std::scientific
            << newton_residuum / old_newton_residuum << '\t';

      if (newton_step==1 || newton_residuum / old_newton_residuum > nonlinear_rho)
        pcout << "rebuild" << '\t';
      else
        pcout << " " << '\t';
      pcout << line_search_step << '\t' << std::scientific
            << no_linear_iterations << '\t' << std::scientific
            << std::endl;

      // Terminate if nothing is solved anymore. After this,
      // we cut the time step.
      if ((newton_residuum/old_newton_residuum > upper_newton_rho) && (newton_step > 1)
         )
        {
          break;
        }



      // Updates
      newton_step++;
    }


  if ((newton_residuum > lower_bound_newton_residuum) && (newton_step == max_no_newton_steps))
    {
      pcout << "Newton iteration did not converge in " << newton_step
            << " steps :-(" << std::endl;
      throw SolverControl::NoConvergence(0,0);
    }

  return newton_residuum/old_newton_residuum;
}

template <int dim>
void
FracturePhaseFieldProblem<dim>::project_back_phase_field ()
{
  typename DoFHandler<dim>::active_cell_iterator cell =
    dof_handler.begin_active(), endc = dof_handler.end();

  std::vector<unsigned int> local_dof_indices(fe.dofs_per_cell);
  for (; cell != endc; ++cell)
    if (cell->is_locally_owned())
      {
        cell->get_dof_indices(local_dof_indices);
        for (unsigned int i=0; i<fe.dofs_per_cell; ++i)
          {
            const unsigned int comp_i = fe.system_to_component_index(i).first;
            if (comp_i != dim)
              continue; // only look at phase field

            const unsigned int idx = local_dof_indices[i];
            if (!dof_handler.locally_owned_dofs().is_element(idx))
              continue;

            solution(idx) = std::max(0.0,
                                     std::min(static_cast<double>(solution(idx)), 1.0));
          }
      }

  solution.compress(VectorOperation::insert);
}



//////////////////
template <int dim>
void
FracturePhaseFieldProblem<dim>::output_results () const
{
  static int refinement_cycle=-1;
  ++refinement_cycle;

  LA::MPI::BlockVector relevant_solution(partition_relevant);
  relevant_solution = solution;

  SneddonExactPostProc<dim> exact_sol_sneddon(alpha_eps);
  DataOut<dim> data_out;
  {
    std::vector<std::string> solution_names(dim, "dis");
    solution_names.push_back("phi");
    std::vector<DataComponentInterpretation::DataComponentInterpretation> data_component_interpretation(
      dim, DataComponentInterpretation::component_is_part_of_vector);
    data_component_interpretation.push_back(DataComponentInterpretation::component_is_scalar);
    data_out.add_data_vector(dof_handler, relevant_solution,
                             solution_names, data_component_interpretation);
  }

  {
    std::vector<std::string> solution_names;
    solution_names.push_back("displacement_x");
    solution_names.push_back("displacement_y");
    solution_names.push_back("phasefieldagain");
    std::vector<DataComponentInterpretation::DataComponentInterpretation> data_component_interpretation(
      dim+1, DataComponentInterpretation::component_is_scalar);
    data_out.add_data_vector(dof_handler, relevant_solution,
                             solution_names, data_component_interpretation);
  }

  if (test_case == TestCase::sneddon_2d)
    {
      data_out.add_data_vector(dof_handler, relevant_solution, exact_sol_sneddon);
    }

  Vector<float> e_mod(triangulation.n_active_cells());
  if (test_case == TestCase::multiple_het)
    {
      typename DoFHandler<dim>::active_cell_iterator cell =
        dof_handler.begin_active(), endc = dof_handler.end();

      std::vector<unsigned int> local_dof_indices(fe.dofs_per_cell);
      unsigned int cellindex = 0;
      for (; cell != endc; ++cell, ++cellindex)
        if (cell->is_locally_owned())
          {
            e_mod(cellindex) = 1.0 + func_emodulus->value(cell->center(), 0);
          }
      data_out.add_data_vector(e_mod, "emodulus");
    }


  Vector<float> subdomain(triangulation.n_active_cells());
  for (unsigned int i = 0; i < subdomain.size(); ++i)
    subdomain(i) = triangulation.locally_owned_subdomain();
  data_out.add_data_vector(subdomain, "subdomain");

  if (outer_solver == OuterSolverType::active_set)
    data_out.add_data_vector(dof_handler, active_set,
                             "active_set");

  data_out.build_patches();

  // Filename basis comes from parameter file
  std::ostringstream filename;

  pcout << "Write solution " << refinement_cycle << std::endl;

  filename << "output/"
           << filename_basis
           << Utilities::int_to_string(refinement_cycle, 5)
           << "."
           << Utilities::int_to_string(triangulation.locally_owned_subdomain(), 4)
           << ".vtu";

  std::ofstream output(filename.str().c_str());
  data_out.write_vtu(output);

  if (Utilities::MPI::this_mpi_process(mpi_com) == 0)
    {
      std::vector<std::string> filenames;
      for (unsigned int i = 0; i < Utilities::MPI::n_mpi_processes(mpi_com);
           ++i)
        filenames.push_back(
          filename_basis + Utilities::int_to_string(refinement_cycle, 5)
          + "." + Utilities::int_to_string(i, 4) + ".vtu");

      std::ofstream master_output(
        ("output/" + filename_basis + Utilities::int_to_string(refinement_cycle, 5)
         + ".pvtu").c_str());
      data_out.write_pvtu_record(master_output, filenames);

      std::string visit_master_filename = ("output/" + filename_basis
                                           + Utilities::int_to_string(refinement_cycle, 5) + ".visit");
      std::ofstream visit_master(visit_master_filename.c_str());
      data_out.write_visit_record(visit_master, filenames);

      static std::vector<std::vector<std::string> > output_file_names_by_timestep;
      output_file_names_by_timestep.push_back(filenames);
      std::ofstream global_visit_master("output/solution.visit");
      data_out.write_visit_record(global_visit_master,
                                  output_file_names_by_timestep);
    }
}

// With help of this function, we extract
// point values for a certain component from our
// discrete solution. We use it to gain the
// displacements of the solid in the x- and y-directions.
template <int dim>
double
FracturePhaseFieldProblem<dim>::compute_point_value (
  const DoFHandler<dim> &dofh, const LA::MPI::BlockVector &vector,
  const Point<dim> &p, const unsigned int component) const
{
  double value = 0.0;
  try
    {
      Vector<double> tmp_vector(dim);
      VectorTools::point_value(dofh, vector, p, tmp_vector);
      value = tmp_vector(component);
    }
  catch (typename VectorTools::ExcPointNotAvailableHere e)
    {
    }

  return Utilities::MPI::sum(value, mpi_com);
}


int value_to_bucket(double x, unsigned int n_buckets)
{
  const double x1 = 1.5;
  const double x2 = 2.5;
  return std::floor((x-x1)/(x2-x1)*n_buckets+0.5);
}

double bucket_to_value(unsigned int idx, unsigned int n_buckets)
{
  const double x1 = 1.5;
  const double x2 = 2.5;
  return x1 + idx*(x2-x1)/n_buckets;
}

template <int dim>
void
FracturePhaseFieldProblem<dim>::compute_cod_array ()
{
  // we want to integrate along dim-1 dim faces along the x axis
  // for this we fill buckets representing slices orthogonal to the x axis
  const unsigned int n_buckets = 75;
  std::vector<double> values(n_buckets);
  std::vector<double> volume(n_buckets);

  std::vector<double> exact(n_buckets);
  for (unsigned int i=0; i<n_buckets; ++i)
    {
      double x = bucket_to_value(i, n_buckets);
      exact[i] = 3.84e-4*std::sqrt(std::max(0.0,1.0-(x-2.0)*(x-2.0)/0.04));
    }

  // this yields 100 quadrature points evenly distributed in the interior of the cell.
  // We avoid points on the faces, as they would be counted more than once.
  const unsigned int n_reps = 50 + 100.0 * min_cell_diameter / (1.0/n_buckets);
  const QIterated<dim> quadrature_formula (QMidpoint<1>(), 100 );
  const unsigned int n_q_points = quadrature_formula.size();

  LA::MPI::BlockVector rel_solution(partition_relevant);
  rel_solution = solution;

  FEValues<dim> fe_values(fe, quadrature_formula,
                          update_values | update_quadrature_points | update_JxW_values
                          | update_gradients);


  typename DoFHandler<dim>::active_cell_iterator cell =
    dof_handler.begin_active(), endc = dof_handler.end();

  std::vector<Vector<double> > solution_values(n_q_points,
                                               Vector<double>(dim+1));

  std::vector<std::vector<Tensor<1, dim> > > solution_grads(
    n_q_points, std::vector<Tensor<1, dim> >(dim+1));


  const double width = bucket_to_value(1, n_buckets) - bucket_to_value(0, n_buckets);

  for (; cell != endc; ++cell)
    if (cell->is_locally_owned())
      {

        fe_values.reinit(cell);

        fe_values.get_function_values(rel_solution,
                                      solution_values);
        fe_values.get_function_gradients(
          rel_solution, solution_grads);

        for (unsigned int q = 0; q < n_q_points; ++q)
          {
            const int idx_ = value_to_bucket(fe_values.quadrature_point(q)[0], n_buckets);
            if (idx_<0 || idx_>=static_cast<int>(n_buckets))
              continue;
            const unsigned int idx = static_cast<unsigned int>(idx_);

            const Tensor<1, dim> u = Tensors::get_u<dim>(
                                       q, solution_values);

            const Tensor<1, dim> grad_pf =
              Tensors::get_grad_pf<dim>(q,
                                        solution_grads);

            double cod_value =
              // Motivated by Bourdin et al. (2012); SPE Paper
              u * grad_pf;

            values[idx] += cod_value * fe_values.JxW(q);
            volume[idx] += fe_values.JxW(q);
          }

      }

  std::vector<double> values_all(n_buckets);
  std::vector<double> volume_all(n_buckets);
  Utilities::MPI::sum(values, mpi_com, values_all);
  Utilities::MPI::sum(volume, mpi_com, volume_all);
  for (unsigned int i=0; i<n_buckets; ++i)
    values[i] = values_all[i] / width / 2.0;

  double middle_value = compute_cod(2.0);

  if (Utilities::MPI::this_mpi_process(mpi_com) == 0)
    {
      static unsigned int no = 0;
      ++no;
      std::ostringstream filename;
      filename <<  "cod-" << Utilities::int_to_string(no, 2) << ".txt";
      pcout << "writing " << filename.str() << std::endl;
      std::ofstream f(filename.str().c_str());

      double error = 0.0;
      for (unsigned int i=0; i<n_buckets; ++i)
        {
          error += std::pow(values[i]-exact[i], 2.0);
          f << bucket_to_value(i, n_buckets) << " " << values[i] << " " << exact[i] << std::endl;
        }
      error = std::sqrt(error);
      double err_middle = std::abs(middle_value-3.84e-4);
      pcout << "ERROR: " << error
            << " alpha_eps: " << alpha_eps
            << " k: " << constant_k
            << " hmin: " << min_cell_diameter
            << " errmiddle: " << err_middle
            << " dofs: " << dof_handler.n_dofs()
            << std::endl;

    }
}

template <int dim>
double
FracturePhaseFieldProblem<dim>::compute_cod (
  const double eval_line)
{

  const QGauss<dim - 1> face_quadrature_formula(3);
  FEFaceValues<dim> fe_face_values(fe, face_quadrature_formula,
                                   update_values | update_quadrature_points | update_gradients
                                   | update_normal_vectors | update_JxW_values);


  LA::MPI::BlockVector rel_solution(partition_relevant);
  rel_solution = solution;

  const unsigned int dofs_per_cell = fe.dofs_per_cell;
  const unsigned int n_face_q_points = face_quadrature_formula.size();

  std::vector<unsigned int> local_dof_indices(dofs_per_cell);

  std::vector<Vector<double> > face_solution_values(n_face_q_points,
                                                    Vector<double>(dim+1));
  std::vector<std::vector<Tensor<1, dim> > > face_solution_grads(
    n_face_q_points, std::vector<Tensor<1, dim> >(dim+1));

  double cod_value = 0.0;
  double eps = 1.0e-6;

  typename DoFHandler<dim>::active_cell_iterator cell =
    dof_handler.begin_active(), endc = dof_handler.end();

  for (; cell != endc; ++cell)
    if (cell->is_locally_owned())
      {
        for (unsigned int face = 0; face < GeometryInfo<dim>::faces_per_cell;
             ++face)
          {
            fe_face_values.reinit(cell, face);
            fe_face_values.get_function_values(rel_solution,
                                               face_solution_values);
            fe_face_values.get_function_gradients(rel_solution,
                                                  face_solution_grads);

            for (unsigned int q_point = 0; q_point < n_face_q_points;
                 ++q_point)
              {
                if ((fe_face_values.quadrature_point(q_point)[0]
                     < (eval_line + eps))
                    && (fe_face_values.quadrature_point(q_point)[0]
                        > (eval_line - eps)))
                  {
                    const Tensor<1, dim> u = Tensors::get_u<dim>(
                                               q_point, face_solution_values);

                    const Tensor<1, dim> grad_pf =
                      Tensors::get_grad_pf<dim>(q_point,
                                                face_solution_grads);

                    // Motivated by Bourdin et al. (2012); SPE Paper
                    cod_value += 0.5 * u * grad_pf
                                 * fe_face_values.JxW(q_point);

                  }

              }
          }
      }

  cod_value = Utilities::MPI::sum(cod_value, mpi_com) / 2.0;

  pcout << eval_line << "  " << cod_value << std::endl;

  return cod_value;

}


template <int dim>
double
FracturePhaseFieldProblem<dim>::compute_energy()
{
  // What are we computing? In Latex-style it is:
  // bulk energy = [(1+k)phi^2 + k] psi(e)
  // crack energy = \frac{G_c}{2}\int_{\Omega}\Bigl( \frac{(\varphi - 1)^2}{\eps}
  //+ \eps |\nabla \varphi|^2 \Bigr) \, dx
  double local_bulk_energy = 0.0;
  double local_crack_energy = 0.0;

  const QGauss<dim> quadrature_formula(degree+2);
  const unsigned int n_q_points = quadrature_formula.size();

  FEValues<dim> fe_values(fe, quadrature_formula,
                          update_values | update_quadrature_points | update_JxW_values
                          | update_gradients);

  typename DoFHandler<dim>::active_cell_iterator cell =
    dof_handler.begin_active(), endc = dof_handler.end();

  LA::MPI::BlockVector rel_solution(partition_relevant);
  rel_solution = solution;

  std::vector<Vector<double> > solution_values(n_q_points,
                                               Vector<double>(dim+1));

  std::vector<std::vector<Tensor<1, dim> > > solution_grads(
    n_q_points, std::vector<Tensor<1, dim> >(dim+1));

  for (; cell != endc; ++cell)
    if (cell->is_locally_owned())
      {
        fe_values.reinit(cell);

        // update lame coefficients based on current cell
        if (test_case == TestCase::multiple_het)
          {
            E_modulus = func_emodulus->value(cell->center(), 0);

            lame_coefficient_mu = E_modulus / (2.0 * (1 + poisson_ratio_nu));

            lame_coefficient_lambda = (2 * poisson_ratio_nu * lame_coefficient_mu)
                                      / (1.0 - 2 * poisson_ratio_nu);
          }

        fe_values.get_function_values(rel_solution, solution_values);
        fe_values.get_function_gradients(rel_solution, solution_grads);

        for (unsigned int q = 0; q < n_q_points; ++q)
          {
            const Tensor<2,dim> grad_u = Tensors
                                         ::get_grad_u<dim> (q, solution_grads);

            const Tensor<2,dim> E = 0.5 * (grad_u + transpose(grad_u));
            const double tr_E = grad_u[0][0] + grad_u[1][1];

            const double pf = solution_values[q](dim);

            const double tr_e_2 = trace(E*E);

            const double psi_e = 0.5 * lame_coefficient_lambda * tr_E*tr_E + lame_coefficient_mu * tr_e_2;

            local_bulk_energy += ((1+constant_k)*pf*pf+constant_k) * psi_e * fe_values.JxW(q);

            local_crack_energy += G_c/2.0 * ((pf-1) * (pf-1)/alpha_eps + alpha_eps * scalar_product(grad_u, grad_u))
                                  * fe_values.JxW(q);
          }

      }

  double bulk_energy = Utilities::MPI::sum(local_bulk_energy, mpi_com);
  double crack_energy = Utilities::MPI::sum(local_crack_energy, mpi_com);

  pcout << "No " << timestep_number << " time " << time
        << " bulk energy: " << bulk_energy
        << " crack energy: " << crack_energy;


  return 0;

}

// Here, we compute the four quantities of interest:
// the x and y-displacements of the structure, the drag, and the lift.
template <int dim>
void
FracturePhaseFieldProblem<dim>::compute_functional_values ()
{
  double lines[] =
  {
    1.0, 1.5, 1.75, 1.78125, 1.8125, 1.84375, 1.875, 1.9375, 2.0, 2.0625, 2.125, 2.15625,
    2.1875, 2.21875, 2.25, 2.5, 3.0
  };
  const unsigned int n_lines = sizeof(lines) / sizeof(*lines);

  static unsigned int no = 0;
  ++no;
  std::ostringstream filename;
  filename <<  "cod-" << Utilities::int_to_string(no, 2) << "b.txt";
  pcout << "writing " << filename.str() << std::endl;

  std::ofstream f(filename.str().c_str());
  for (unsigned int i = 0; i < n_lines; ++i)
    {
      double value = compute_cod(lines[i]);
      f << lines[i] << " " << value << std::endl;
    }

//    double y_and_h = 2.0 + min_cell_diameter;
//    double px[] = { 1.6, 1.8, 1.85, 1.9, 1.95, 2.0, 2.05, 2.1, 2.15, 2.2, 2.4 };
//    const unsigned int n = sizeof(px) / sizeof(*px);
//    std::vector<double> val(n);


}


template <int dim>
void
FracturePhaseFieldProblem<dim>::compute_load ()
{
  const QGauss<dim-1> face_quadrature_formula (3);
  FEFaceValues<dim> fe_face_values (fe, face_quadrature_formula,
                                    update_values | update_gradients | update_normal_vectors |
                                    update_JxW_values);

  const unsigned int dofs_per_cell = fe.dofs_per_cell;
  const unsigned int n_face_q_points = face_quadrature_formula.size();

  std::vector<unsigned int> local_dof_indices (dofs_per_cell);

  std::vector<std::vector<Tensor<1,dim> > >
  face_solution_grads (n_face_q_points, std::vector<Tensor<1,dim> > (dim+1));

  Tensor<1,dim> load_value;

  LA::MPI::BlockVector rel_solution(partition_relevant);
  rel_solution = solution;

  const Tensor<2, dim> Identity =
    Tensors::get_Identity<dim>();

  typename DoFHandler<dim>::active_cell_iterator
  cell = dof_handler.begin_active(),
  endc = dof_handler.end();

  for (; cell!=endc; ++cell)
    if (cell->is_locally_owned())
      {
        for (unsigned int face=0; face<GeometryInfo<dim>::faces_per_cell; ++face)
          if (cell->face(face)->at_boundary() &&
              cell->face(face)->boundary_indicator()==3)
            {
              fe_face_values.reinit (cell, face);
              fe_face_values.get_function_gradients (rel_solution, face_solution_grads);

              for (unsigned int q_point=0; q_point<n_face_q_points; ++q_point)
                {
                  const Tensor<2, dim> grad_u
                    = Tensors::get_grad_u<dim>(q_point, face_solution_grads);

                  const Tensor<2, dim> E = 0.5 * (grad_u + transpose(grad_u));
                  const double tr_E = grad_u[0][0] + grad_u[1][1];

                  Tensor<2, dim> stress_term;
                  stress_term = lame_coefficient_lambda * tr_E * Identity
                                + 2 * lame_coefficient_mu * E;

                  load_value +=  stress_term *
                                 fe_face_values.normal_vector(q_point)* fe_face_values.JxW(q_point);

                }
            } // end boundary 3 for structure


      }


  load_value[0] *= -1.0;

  if (test_case == TestCase::miehe_tension)
    {
      pcout << "  Load y: " << Utilities::MPI::sum(load_value[1], mpi_com) << std::endl;
    }
  else if (test_case == TestCase::miehe_shear)
    {
      pcout << "  Load x: " << Utilities::MPI::sum(load_value[0], mpi_com) << std::endl;
    }
}

// Determine the phase-field regularization parameters
// eps and kappa
template <int dim>
void
FracturePhaseFieldProblem<dim>::determine_mesh_dependent_parameters()
{
  min_cell_diameter = 1.0e+300;
  {
    typename DoFHandler<dim>::active_cell_iterator cell =
      dof_handler.begin_active(), endc = dof_handler.end();

    for (; cell != endc; ++cell)
      if (cell->is_locally_owned())
        {
          min_cell_diameter = std::min(cell->diameter(), min_cell_diameter);
        }

    min_cell_diameter = -Utilities::MPI::max(-min_cell_diameter, mpi_com);
  }

  // for this test we want to use the h that will be used at the end
  if (test_case == TestCase::miehe_tension
      || test_case == TestCase::miehe_shear
      || test_case == TestCase::multiple_homo
      || test_case == TestCase::multiple_het)
    {
      min_cell_diameter = dof_handler.begin(0)->diameter();
      min_cell_diameter *= std::pow(2.0,-1.0*(n_global_pre_refine+n_refinement_cycles+n_local_pre_refine));
    }

  // Set additional runtime parameters, the
  // regularization parameters, which
  // are chosen dependent on the present mesh size
  // old relations (see below why now others are used!)

  bool h_and_eps_small_o = false;
  if (h_and_eps_small_o && test_case == TestCase::sneddon_2d)
    {
      // Bourdin 1999 Image Segmentation gives ideas for
      // choice of parameters w.r.t. h
      // also used in Mikelic, Wheeler, Wick (ICES reports 13-15, 14-18)
      // However, this if-control statement is now redundant since
      // we also adapt the parameters in the parameter file.
      // I kept them because this specific relation was used
      // in our CMAME paper (2013).
      constant_k = 0.25 * std::sqrt(min_cell_diameter);
      alpha_eps = 0.5 * std::sqrt(constant_k);
    }
  else
    {
      FunctionParser<1> func;
      prm.enter_subsection("Problem dependent parameters");
      func.initialize("h", prm.get("K reg"), std::map<std::string, double>());
      constant_k = func.value(Point<1>(min_cell_diameter), 0);
      func.initialize("h", prm.get("Eps reg"), std::map<std::string, double>());
      alpha_eps = func.value(Point<1>(min_cell_diameter), 0);
      prm.leave_subsection();
    }

  // sanity check
//   pcout << "****"
//   << "h = " << min_cell_diameter
//   << "k = " << constant_k
//   << " alpha_eps = " << alpha_eps
//   << std::endl;

}


template <int dim>
bool
FracturePhaseFieldProblem<dim>::refine_mesh ()
{
  LA::MPI::BlockVector relevant_solution(partition_relevant);
  relevant_solution = solution;

  if (refinement_strategy == RefinementStrategy::fixed_preref_sneddon)
    {
      typename DoFHandler<dim>::active_cell_iterator cell =
        dof_handler.begin_active(), endc = dof_handler.end();

      for (; cell != endc; ++cell)
        if (cell->is_locally_owned())
          {

            for (unsigned int vertex = 0;
                 vertex < GeometryInfo<dim>::vertices_per_cell; ++vertex)
              {
                Tensor<1, dim> cell_vertex = (cell->vertex(vertex));
                if (cell_vertex[0] <= 2.5 && cell_vertex[0] >= 1.5
                    && cell_vertex[1] <= 2.25 && cell_vertex[1] >= 1.75)
                  {
                    cell->set_refine_flag();
                    break;
                  }
              }

          }
    }    // end Sneddon
  else if (refinement_strategy == RefinementStrategy::fixed_preref_miehe_tension)
    {
      typename DoFHandler<dim>::active_cell_iterator cell =
        dof_handler.begin_active(), endc = dof_handler.end();

      for (; cell != endc; ++cell)
        if (cell->is_locally_owned())
          {

            for (unsigned int vertex = 0;
                 vertex < GeometryInfo<dim>::vertices_per_cell; ++vertex)
              {
                Tensor<1, dim> cell_vertex = (cell->vertex(vertex));
                if (cell_vertex[0] <= 0.6 && cell_vertex[0] >= 0.0
                    && cell_vertex[1] <= 0.55 && cell_vertex[1] >= 0.45)
                  {
                    cell->set_refine_flag();
                    break;
                  }
              }

          }
    }    // end Miehe tension
  else if (refinement_strategy == RefinementStrategy::fixed_preref_miehe_shear)
    {
      typename DoFHandler<dim>::active_cell_iterator cell =
        dof_handler.begin_active(), endc = dof_handler.end();

      for (; cell != endc; ++cell)
        if (cell->is_locally_owned())
          {

            for (unsigned int vertex = 0;
                 vertex < GeometryInfo<dim>::vertices_per_cell; ++vertex)
              {
                Tensor<1, dim> cell_vertex = (cell->vertex(vertex));
                if (cell_vertex[0] <= 0.6 && cell_vertex[0] >= 0.0
                    && cell_vertex[1] <= 0.55 && cell_vertex[1] >= 0.0)
                  {
                    cell->set_refine_flag();
                    break;
                  }
              }

          }
    }    // end Miehe shear
  else if (refinement_strategy == RefinementStrategy::phase_field_ref)
    {
      // refine if phase field < constant
      typename DoFHandler<dim>::active_cell_iterator cell =
        dof_handler.begin_active(), endc = dof_handler.end();
      std::vector<unsigned int> local_dof_indices(fe.dofs_per_cell);

      for (; cell != endc; ++cell)
        if (cell->is_locally_owned())
          {
            cell->get_dof_indices(local_dof_indices);
            for (unsigned int i=0; i<fe.dofs_per_cell; ++i)
              {
                const unsigned int comp_i = fe.system_to_component_index(i).first;
                if (comp_i != dim)
                  continue; // only look at phase field
                if (relevant_solution(local_dof_indices[i])
                    < value_phase_field_for_refinement )
                  {
                    cell->set_refine_flag();
                    break;
                  }
              }
          }
    }
  else if (refinement_strategy == RefinementStrategy::global)
    {
      typename DoFHandler<dim>::active_cell_iterator cell =
        dof_handler.begin_active(), endc = dof_handler.end();
      for (; cell != endc; ++cell)
        if (cell->is_locally_owned())
          cell->set_refine_flag();
    }
  else if (refinement_strategy == RefinementStrategy::mix)
    {

      {
        typename DoFHandler<dim>::active_cell_iterator cell =
          dof_handler.begin_active(), endc = dof_handler.end();
        std::vector<unsigned int> local_dof_indices(fe.dofs_per_cell);

        for (; cell != endc; ++cell)
          if (cell->is_locally_owned())
            {
              cell->get_dof_indices(local_dof_indices);
              for (unsigned int i=0; i<fe.dofs_per_cell; ++i)
                {
                  const unsigned int comp_i = fe.system_to_component_index(i).first;
                  if (comp_i != dim)
                    continue; // only look at phase field
                  if (relevant_solution(local_dof_indices[i])
                      < value_phase_field_for_refinement )
                    {
                      cell->set_refine_flag();
                      break;
                    }
                }
            }
      }

      Vector<float> estimated_error_per_cell (triangulation.n_active_cells());
      std::vector<bool> component_mask(dim+1, true);
      component_mask[dim] = false;

      // estimate displacement:
      KellyErrorEstimator<dim>::estimate (dof_handler,
                                          QGauss<dim-1>(degree+2),
                                          typename FunctionMap<dim>::type(),
                                          relevant_solution,
                                          estimated_error_per_cell,
                                          component_mask,
                                          0,
                                          0,
                                          triangulation.locally_owned_subdomain());

      // but ignore cells in the crack:
      {
        typename DoFHandler<dim>::active_cell_iterator cell =
          dof_handler.begin_active(), endc = dof_handler.end();
        std::vector<unsigned int> local_dof_indices(fe.dofs_per_cell);

        unsigned int idx = 0;
        for (; cell != endc; ++cell, ++idx)
          if (cell->refine_flag_set())
            estimated_error_per_cell[idx] = 0.0;
      }

      parallel::distributed::GridRefinement::
      refine_and_coarsen_fixed_number (triangulation,
                                       estimated_error_per_cell,
                                       0.3, 0.0);


    }



  // limit level
  if (test_case != TestCase::sneddon_2d)
    {
      typename DoFHandler<dim>::active_cell_iterator cell =
        dof_handler.begin_active(), endc = dof_handler.end();
      for (; cell != endc; ++cell)
        if (cell->is_locally_owned()
            && cell->level() == static_cast<int>(n_global_pre_refine+n_refinement_cycles+n_local_pre_refine))
          cell->clear_refine_flag();
    }

  // check if we are doing anything
  {
    bool refine_or_coarsen = false;
    triangulation.prepare_coarsening_and_refinement();

    typename DoFHandler<dim>::active_cell_iterator cell =
      dof_handler.begin_active(), endc = dof_handler.end();
    for (; cell != endc; ++cell)
      if (cell->is_locally_owned() &&
          (cell->refine_flag_set() || cell->coarsen_flag_set()))
        {
          refine_or_coarsen = true;
          break;
        }

    if (Utilities::MPI::sum(refine_or_coarsen?1:0, mpi_com)==0)
      return false;
  }

  std::vector<const LA::MPI::BlockVector *> x(3);
  x[0] = &relevant_solution;
  x[1] = &old_solution;
  x[2] = &old_old_solution;

  parallel::distributed::SolutionTransfer<dim, LA::MPI::BlockVector> solution_transfer(
    dof_handler);

  solution_transfer.prepare_for_coarsening_and_refinement(x);

  triangulation.execute_coarsening_and_refinement();
  setup_system();

  LA::MPI::BlockVector tmp_v(partition);
  LA::MPI::BlockVector tmp_vv(partition);
  std::vector<LA::MPI::BlockVector *> tmp(3);
  tmp[0] = &solution;
  tmp[1] = &tmp_v;
  tmp[2] = &tmp_vv;

  solution_transfer.interpolate(tmp);
  old_solution = tmp_v;
  old_old_solution = tmp_vv;

  determine_mesh_dependent_parameters();
  return true;
}

// As usual, we have to call the run method.
template <int dim>
void
FracturePhaseFieldProblem<dim>::run ()
{
  pcout << "Running on " << Utilities::MPI::n_mpi_processes(mpi_com)
        << " cores" << std::endl;

  set_runtime_parameters();
  setup_system();

  for (unsigned int i = 0; i < n_local_pre_refine; ++i)
    {
      ConstraintMatrix constraints;
      constraints.close();

      if (test_case == TestCase::sneddon_2d)
        {
          VectorTools::interpolate(dof_handler,
                                   InitialValuesSneddon<dim>(min_cell_diameter), solution);

        }
      else if (test_case == TestCase::multiple_homo)
        {
          VectorTools::interpolate(dof_handler,
                                   InitialValuesMultipleHomo<dim>(min_cell_diameter), solution);

        }
      else if (test_case == TestCase::multiple_het)
        {
          VectorTools::interpolate(dof_handler,
                                   InitialValuesMultipleHet<dim>(min_cell_diameter), solution);

        }
      else
        {
          VectorTools::interpolate(dof_handler,
                                   InitialValuesMiehe<dim>(min_cell_diameter), solution);
        }
      refine_mesh();

    }

  if (n_local_pre_refine==0)
    determine_mesh_dependent_parameters();

  AssertThrow(alpha_eps >= min_cell_diameter, ExcMessage("You need to pick eps >= h"));
  AssertThrow(constant_k < 1.0, ExcMessage("You need to pick K < 1"));

  pcout << "\n=============================="
        << "=====================================" << std::endl;
  pcout << "Parameters\n" << "==========\n" << "h (min):           "
        << min_cell_diameter << "\n" << "k:                 " << constant_k
        << "\n" << "eps:               " << alpha_eps << "\n"
        << "G_c:               " << G_c << "\n"
        << "gamma penal:       " << gamma_penal << "\n"
        << "Poisson nu:        " << poisson_ratio_nu << "\n"
        << "E modulus:         " << E_modulus << "\n"
        << "Lame mu:           " << lame_coefficient_mu << "\n"
        << "Lame lambda:       " << lame_coefficient_lambda << "\n"
        << std::endl;


  {
    ConstraintMatrix constraints;
    constraints.close();

    if (test_case == TestCase::sneddon_2d)
      {
        VectorTools::interpolate(dof_handler,
                                 InitialValuesSneddon<dim>(min_cell_diameter), solution);
      }
    else if (test_case == TestCase::multiple_homo)
      {
        VectorTools::interpolate(dof_handler,
                                 InitialValuesMultipleHomo<dim>(min_cell_diameter), solution);

      }
    else if (test_case == TestCase::multiple_het)
      {
        VectorTools::interpolate(dof_handler,
                                 InitialValuesMultipleHet<dim>(min_cell_diameter), solution);

      }
    else
      {
        VectorTools::interpolate(dof_handler,
                                 InitialValuesMiehe<dim>(min_cell_diameter), solution);
      }
    output_results();
  }

  // Normalize phase-field function between 0 and 1
  project_back_phase_field();

  const unsigned int output_skip = 1;
  unsigned int refinement_cycle = 0;
  double finishing_timestep_loop = 0;
  double tmp_timestep = 0.0;

  // Initialize old and old_old_solutions
  // old_old is needed for extrapolation for pf_extra to avoid pf^2 in block(0,0)
  old_old_solution = solution;
  old_solution = solution;

  // Initialize old and old_old timestep sizes
  old_timestep = timestep;
  old_old_timestep = timestep;

  // Timestep loop
  do
    {

      {
        //begin timer
        TimerOutput::Scope t(timer, "Time step loop");

        double newton_reduction = 1.0;


        if (timestep_number > switch_timestep && switch_timestep>0)
          timestep = timestep_size_2;

        tmp_timestep = timestep;
        old_old_timestep = old_timestep;
        old_timestep = timestep;

        // Compute next time step
        old_old_solution = old_solution;
        old_solution = solution;

redo_step:
        pcout << std::endl;
        pcout << "\n=============================="
              << "=========================================" << std::endl;
        pcout << "Timestep " << timestep_number << ": " << time << " (" << timestep << ")"
              << "   " << "Cells: " << triangulation.n_global_active_cells()
              << "   " << "DoFs: " << dof_handler.n_dofs();
        pcout << "\n--------------------------------"
              << "---------------------------------------" << std::endl;

        pcout << std::endl;

        if (outer_solver == OuterSolverType::active_set)
          {
            time += timestep;
            do
              {
                // The Newton method can either stagnate or the linear solver
                // might not converge. To not abort the program we catch the
                // exception and retry with a smaller step.
                use_old_timestep_pf = false;
                try
                  {
                    newton_reduction = newton_active_set();

                    break;

                  }
                catch (SolverControl::NoConvergence e)
                  {
                    pcout << "Solver did not converge! Adjusting time step to " << timestep/10 << std::endl;
                  }

                pcout << "Nehme nun old_timestep_pf" << std::endl;
                use_old_timestep_pf = true;
                solution = old_solution;

                // Time step cut
                time -= timestep;
                timestep = timestep/10.0;
                time += timestep;

              }
            while (true);
          }
        else if (outer_solver == OuterSolverType::simple_monolithic)
          {
            // Increment time
            time += timestep;

            do
              {
                // The Newton method can either stagnate or the linear solver
                // might not converge. To not abort the program we catch the
                // exception and retry with a smaller step.
                use_old_timestep_pf = false;
                try
                  {
                    // Normalize phase-field function between 0 and 1
                    project_back_phase_field();
                    newton_reduction = newton_iteration(time);

                    while (newton_reduction > upper_newton_rho)
                      {
                        use_old_timestep_pf = true;
                        time -= timestep;
                        timestep = timestep/10.0;
                        time += timestep;
                        solution = old_solution;
                        newton_reduction = newton_iteration (time);

                        if (timestep < 1.0e-9)
                          {
                            pcout << "Timestep too small - taking step" << std::endl;
                            break;
                          }
                      }

                    break;


                  }
                catch (SolverControl::NoConvergence e)
                  {
                    pcout << "Solver did not converge! Adjusting time step." << std::endl;
                  }

                time -= timestep;
                solution = old_solution;
                timestep = timestep/10.0;
                time += timestep;

              }
            while (true);

          }
        else throw ExcNotImplemented();

        // Normalize phase-field function between 0 and 1
        // TW: I think this function is not really needed any more
        project_back_phase_field();
        constraints_hanging_nodes.distribute(solution);

        if (test_case != TestCase::sneddon_2d)
          {
            bool changed = refine_mesh();
            if (changed)
              {
                // redo the current time step
                pcout << "MESH CHANGED!" << std::endl;
                time -= timestep;
                solution = old_solution;
                goto redo_step;
                continue;
              }
          }

        // Set timestep to original timestep
        timestep = tmp_timestep;

        // Compute functional values
        pcout << std::endl;
        compute_energy();
        if (test_case == TestCase::sneddon_2d ||
            test_case == TestCase::multiple_homo ||
            test_case == TestCase::multiple_het)
          {
            pcout << std::endl;
            //They are computed below
            //compute_functional_values();
            //compute_cod_array();
          }
        else
          compute_load();



        // Write solutions
        if ((timestep_number % output_skip == 0))
          output_results();

        // is this the residual? rename variable if not
        LA::MPI::BlockVector residual(partition);
        residual = old_solution;
        residual.add(-1.0, solution);

        // Abbruchkriterium time step algorithm
        finishing_timestep_loop = residual.linfty_norm();
        if (test_case == TestCase::sneddon_2d)
          pcout << "Timestep difference linfty: " << finishing_timestep_loop << std::endl;

        ++timestep_number;

        if (test_case == TestCase::sneddon_2d && finishing_timestep_loop < 1.0e-5)
          {
            //compute_cod_array();
            compute_functional_values();

            // Now we compare phi to our reference function
            {
              ExactPhiSneddon<dim> exact(alpha_eps);
              Vector<float> error (triangulation.n_active_cells());

              LA::MPI::BlockVector rel_solution(
                partition_relevant);
              rel_solution = solution;

              ComponentSelectFunction<dim> value_select (dim, dim+1);
              VectorTools::integrate_difference (dof_handler,
                                                 rel_solution,
                                                 exact,
                                                 error,
                                                 QGauss<dim>(fe.degree+2),
                                                 VectorTools::L2_norm,
                                                 &value_select);
              const double local_error = error.l2_norm();
              const double L2_error =  std::sqrt( Utilities::MPI::sum(local_error * local_error, mpi_com));
              pcout << "phi_L2_error: " << L2_error << " h: " << min_cell_diameter << std::endl;
            }

            if (n_refinement_cycles==0)
              break;

            --n_refinement_cycles;
            //timestep_number = 0;
            pcout << std::endl;
            pcout  << "\n================== " << std::endl;
            pcout << "Refinement cycle " << refinement_cycle
                  << "\n------------------ " << std::endl;

            refine_mesh();
            solution = 0;
            ++refinement_cycle;
            if (test_case == TestCase::sneddon_2d)
              {
                VectorTools::interpolate(dof_handler,
                                         InitialValuesSneddon<dim>(min_cell_diameter), solution);
              }
            else if (test_case == TestCase::multiple_homo)
              {
                VectorTools::interpolate(dof_handler,
                                         InitialValuesMultipleHomo<dim>(min_cell_diameter), solution);

              }
            else if (test_case == TestCase::multiple_het)
              {
                VectorTools::interpolate(dof_handler,
                                         InitialValuesMultipleHet<dim>(min_cell_diameter), solution);

              }
            else
              VectorTools::interpolate(dof_handler,
                                       InitialValuesMiehe<dim>(min_cell_diameter), solution);

          }


      } // end timer

    }
  while (timestep_number <= max_no_timesteps);

  pcout << std::endl;
  pcout << "Finishing time step loop: " << finishing_timestep_loop
        << std::endl;

  pcout << std::resetiosflags(std::ios::floatfield) << std::fixed;
  std::cout.precision(2);

  Utilities::System::MemoryStats stats;
  Utilities::System::get_memory_stats(stats);
  pcout << "VMPEAK, Resident in kB: " << stats.VmSize << " " << stats.VmRSS
        << std::endl;
}

// The main function looks almost the same
// as in all other deal.II tuturial steps.
int
main (
  int argc, char *argv[])
{
  Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv, 1);
  try
    {
      deallog.depth_console(0);

      ParameterHandler prm;
      FracturePhaseFieldProblem<2>::declare_parameters(prm);
      if (argc>1)
        {
          if (!prm.read_input(argv[1], true))
            AssertThrow(false, ExcMessage("could not read .prm!"));
        }
      else
        {
          std::ofstream out("default.prm");
          prm.print_parameters (out,
                                ParameterHandler::Text);
          std::cout << "usage: ./cracks <parameter_file>" << std::endl
                    << " (created default.prm)" << std::endl;
          return 0;
        }


      FracturePhaseFieldProblem<2> fracture_problem(1, prm);
      fracture_problem.run();
    }
  catch (std::exception &exc)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Exception on processing: " << std::endl << exc.what()
                << std::endl << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;

      return 1;
    }
  catch (...)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Unknown exception!" << std::endl << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }

  return 0;
}

