// Copyright 2019 PaGMO development team
//
// This file is part of the pygmo library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <boost/numeric/conversion/cast.hpp>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <pagmo/algorithm.hpp>
#include <pagmo/archipelago.hpp>
#include <pagmo/batch_evaluators/default_bfe.hpp>
#include <pagmo/batch_evaluators/member_bfe.hpp>
#include <pagmo/batch_evaluators/thread_bfe.hpp>
#include <pagmo/bfe.hpp>
#include <pagmo/detail/gte_getter.hpp>
#include <pagmo/detail/make_unique.hpp>
#include <pagmo/exceptions.hpp>
#include <pagmo/island.hpp>
#include <pagmo/islands/thread_island.hpp>
#include <pagmo/population.hpp>
#include <pagmo/r_policy.hpp>
#include <pagmo/rng.hpp>
#include <pagmo/s11n.hpp>
#include <pagmo/s_policy.hpp>
#include <pagmo/threading.hpp>
#include <pagmo/types.hpp>

#include "algorithm.hpp"
#include "bfe.hpp"
#include "common_utils.hpp"
#include "docstrings.hpp"
#include "expose_algorithms.hpp"
#include "expose_bfes.hpp"
#include "expose_islands.hpp"
#include "expose_problems.hpp"
#include "expose_r_policies.hpp"
#include "island.hpp"
#include "object_serialization.hpp"
#include "problem.hpp"
#include "r_policy.hpp"

namespace py = pybind11;
namespace pg = pagmo;

namespace pygmo
{

namespace detail
{

namespace
{

// NOTE: we need to provide a custom raii waiter in the island. The reason is the following.
// When we call wait() from Python, the calling thread will be holding the GIL and then we will be waiting
// for evolutions in the island to finish. During this time, no
// Python code will be executed because the GIL is locked. This means that if we have a Python thread doing background
// work (e.g., managing the task queue in pythonic islands), it will have to wait before doing any progress. By
// unlocking the GIL before calling thread_island::wait(), we give the chance to other Python threads to continue
// doing some work.
// NOTE: here we have 2 RAII classes interacting with the GIL. The GIL releaser is the *second* one,
// and it is the one that is responsible for unlocking the Python interpreter while wait() is running.
// The *first* one, the GIL thread ensurer, does something else: it makes sure that we can call the Python
// interpreter from the current C++ thread. In a normal situation, in which islands are just instantiated
// from the main thread, the gte object is superfluous. However, if we are interacting with islands from a
// separate C++ thread, then we need to make sure that every time we call into the Python interpreter (e.g., by
// using the GIL releaser below) we inform Python we are about to call from a separate thread. This is what
// the GTE object does. This use case is, for instance, what happens with the PADE algorithm when, algo, prob,
// etc. are all C++ objects (when at least one object is pythonic, we will not end up using the thread island).
// NOTE: by ordering the class members in this way we ensure that gte is constructed before gr, which is essential
// (otherwise we might be calling into the interpreter with a releaser before informing Python we are calling
// from a separate thread).
struct py_wait_locks {
    gil_thread_ensurer gte;
    gil_releaser gr;
};

// Serialization helpers for population.
py::tuple population_pickle_getstate(const pg::population &pop)
{
    std::ostringstream oss;
    {
        boost::archive::binary_oarchive oarchive(oss);
        oarchive << pop;
    }
    auto s = oss.str();
    return py::make_tuple(py::bytes(s.data(), boost::numeric_cast<py::size_t>(s.size())));
}

pg::population population_pickle_setstate(py::tuple state)
{
    if (py::len(state) != 1) {
        py_throw(PyExc_ValueError, ("the state tuple passed for population deserialization "
                                    "must have 1 element, but instead it has "
                                    + std::to_string(py::len(state)) + " element(s)")
                                       .c_str());
    }

    auto ptr = PyBytes_AsString(state[0].ptr());
    if (!ptr) {
        py_throw(PyExc_TypeError, "a bytes object is needed to deserialize a population");
    }

    std::istringstream iss;
    iss.str(std::string(ptr, ptr + py::len(state[0])));
    pg::population pop;
    {
        boost::archive::binary_iarchive iarchive(iss);
        iarchive >> pop;
    }

    return pop;
}

// Test that the serialization of pybind11 objects works as expected.
// The object returned by this function should be identical to the input
// object.
py::object test_object_serialization(const py::object &o)
{
    std::ostringstream oss;
    {
        boost::archive::binary_oarchive oarchive(oss);
        oarchive << object_to_vchar(o);
    }
    const std::string tmp_str = oss.str();
    std::istringstream iss;
    iss.str(tmp_str);
    py::object retval;
    {
        boost::archive::binary_iarchive iarchive(iss);
        std::vector<char> tmp;
        iarchive >> tmp;
        retval = vchar_to_object(tmp);
    }
    return retval;
}

} // namespace

} // namespace detail

} // namespace pygmo

PYBIND11_MODULE(core, m)
{
    // This function needs to be called before doing anything with threads.
    // https://docs.python.org/3/c-api/init.html
    PyEval_InitThreads();

    // Disable automatic function signatures in the docs.
    // NOTE: the 'options' object needs to stay alive
    // throughout the whole definition of the module.
    py::options options;
    options.disable_function_signatures();

    // Expose some internal functions for testing.
    m.def("_callable", &pygmo::callable);
    m.def("_callable_attribute", &pygmo::callable_attribute);
    m.def("_str", &pygmo::str);
    m.def("_type", &pygmo::type);
    m.def("_builtins", &pygmo::builtins);
    m.def("_deepcopy", &pygmo::deepcopy);
    m.def("_test_object_serialization", &pygmo::detail::test_object_serialization);

    // The random_device_next() helper.
    m.def("_random_device_next", []() { return pg::random_device::next(); });

    // Global random number generator
    m.def(
        "set_global_rng_seed", [](unsigned seed) { pg::random_device::set_seed(seed); },
        pygmo::set_global_rng_seed_docstring().c_str(), py::arg("seed"));

    // Override the default implementation of the island factory.
    pg::detail::island_factory
        = [](const pg::algorithm &algo, const pg::population &pop, std::unique_ptr<pg::detail::isl_inner_base> &ptr) {
              if (algo.get_thread_safety() >= pg::thread_safety::basic
                  && pop.get_problem().get_thread_safety() >= pg::thread_safety::basic) {
                  // Both algo and prob have at least the basic thread safety guarantee. Use the thread island.
                  ptr = pg::detail::make_unique<pg::detail::isl_inner<pg::thread_island>>();
              } else {
                  // NOTE: here we are re-implementing a piece of code that normally
                  // is pure C++. We are calling into the Python interpreter, so, in order to handle
                  // the case in which we are invoking this code from a separate C++ thread, we construct a GIL ensurer
                  // in order to guard against concurrent access to the interpreter. The idea here is that this piece
                  // of code normally would provide a basic thread safety guarantee, and in order to continue providing
                  // it we use the ensurer.
                  pygmo::gil_thread_ensurer gte;
                  auto py_island = py::module::import("pygmo").attr("mp_island");
                  ptr = pg::detail::make_unique<pg::detail::isl_inner<py::object>>(py_island());
              }
          };

    // Override the default implementation of default_bfe.
    pg::detail::default_bfe_impl = [](const pg::problem &p, const pg::vector_double &dvs) -> pg::vector_double {
        // The member function batch_fitness() of p, if present, has priority.
        if (p.has_batch_fitness()) {
            return pg::member_bfe{}(p, dvs);
        }

        // Otherwise, we run the generic thread-based bfe, if the problem
        // is thread-safe enough.
        if (p.get_thread_safety() >= pg::thread_safety::basic) {
            return pg::thread_bfe{}(p, dvs);
        }

        // NOTE: in this last bit of the implementation we need to call
        // into the Python interpreter. In order to ensure that default_bfe
        // still works also from a C++ thread of which Python knows nothing about,
        // we will be using a thread ensurer, so that the thread safety
        // guarantee provided by default_bfe is still respected.
        // NOTE: the original default_bfe code is thread safe in the sense that the
        // code directly implemented within that class is thread safe. Invoking the call
        // operator of default_bfe might still end up being thread unsafe if p
        // itself is thread unsafe (the same happens, e.g., in a thread-safe algorithm
        // which uses a thread-unsafe problem in its evolve()).
        pygmo::gil_thread_ensurer gte;
        // Otherwise, we go for the multiprocessing bfe.
        return pygmo::ndarr_to_vector<pg::vector_double>(
            py::cast<py::array_t<double>>(py::module::import("pygmo").attr("mp_bfe")().attr("__call__")(
                p, pygmo::vector_to_ndarr<py::array_t<double>>(dvs))));
    };

    // Override the default RAII waiter. We need to use shared_ptr because we don't want to move/copy/destroy
    // the locks when invoking this from island::wait(), we need to instaniate exactly 1 py_wait_lock and have it
    // destroyed at the end of island::wait().
    pg::detail::wait_raii_getter = []() { return std::make_shared<pygmo::detail::py_wait_locks>(); };

    // NOTE: set the gte getter.
    pg::detail::gte_getter = []() { return std::make_shared<pygmo::gil_thread_ensurer>(); };

    // Register pagmo's custom exceptions.
    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) {
                std::rethrow_exception(p);
            }
        } catch (const pg::not_implemented_error &nie) {
            PyErr_SetString(PyExc_NotImplementedError, nie.what());
        }
    });

    // The thread_safety enum.
    py::enum_<pg::thread_safety>(m, "thread_safety")
        .value("none", pg::thread_safety::none)
        .value("basic", pg::thread_safety::basic)
        .value("constant", pg::thread_safety::constant);

    // The evolve_status enum.
    py::enum_<pg::evolve_status>(m, "evolve_status")
        .value("idle", pg::evolve_status::idle)
        .value("busy", pg::evolve_status::busy)
        .value("idle_error", pg::evolve_status::idle_error)
        .value("busy_error", pg::evolve_status::busy_error);

    // Migration type enum.
    py::enum_<pg::migration_type>(m, "migration_type")
        .value("p2p", pg::migration_type::p2p)
        .value("broadcast", pg::migration_type::broadcast);

    // Migrant handling policy enum.
    py::enum_<pg::migrant_handling>(m, "migrant_handling")
        .value("preserve", pg::migrant_handling::preserve)
        .value("evict", pg::migrant_handling::evict);

    // Add the submodules.
    auto problems_module = m.def_submodule("problems");
    auto algorithms_module = m.def_submodule("algorithms");
    auto islands_module = m.def_submodule("islands");
    auto batch_evaluators_module = m.def_submodule("batch_evaluators");
    auto topologies_module = m.def_submodule("topologies");
    auto r_policies_module = m.def_submodule("r_policies");
    auto s_policies_module = m.def_submodule("s_policies");

    // Population class.
    py::class_<pg::population> pop_class(m, "population", pygmo::population_docstring().c_str());
    pop_class
        // Def ctor.
        .def(py::init<>())
        // Ctors from problem.
        // NOTE: we expose only the ctors from pagmo::problem, not from C++ or Python UDPs. An __init__ wrapper
        // on the Python side will take care of cting a pagmo::problem from the input UDP, and then invoke this ctor.
        // This way we avoid having to expose a different ctor for every exposed C++ prob. Same idea with
        // the bfe argument.
        .def(py::init<const pg::problem &, pg::population::size_type, unsigned>())
        .def(py::init<const pg::problem &, const pg::bfe &, pg::population::size_type, unsigned>())
        // repr().
        .def("__repr__", &pygmo::ostream_repr<pg::population>)
        // Copy and deepcopy.
        .def("__copy__", &pygmo::generic_copy_wrapper<pg::population>)
        .def("__deepcopy__", &pygmo::generic_deepcopy_wrapper<pg::population>)
        // Pickle support.
        .def(py::pickle(&pygmo::detail::population_pickle_getstate, &pygmo::detail::population_pickle_setstate))
        .def(
            "push_back",
            [](pg::population &pop, const py::array_t<double> &x, const py::object &f) {
                if (f.is_none()) {
                    pop.push_back(pygmo::ndarr_to_vector<pg::vector_double>(x));
                } else {
                    pop.push_back(pygmo::ndarr_to_vector<pg::vector_double>(x),
                                  pygmo::ndarr_to_vector<pg::vector_double>(py::cast<py::array_t<double>>(f)));
                }
            },
            pygmo::population_push_back_docstring().c_str(), py::arg("x"), py::arg("f") = py::none())
        .def(
            "random_decision_vector",
            [](const pg::population &pop) {
                return pygmo::vector_to_ndarr<py::array_t<double>>(pop.random_decision_vector());
            },
            pygmo::population_random_decision_vector_docstring().c_str())
        .def(
            "best_idx",
            [](const pg::population &pop, const py::array_t<double> &tol) {
                return pop.best_idx(pygmo::ndarr_to_vector<pg::vector_double>(tol));
            },
            py::arg("tol"))
        .def(
            "best_idx", [](const pg::population &pop, double tol) { return pop.best_idx(tol); }, py::arg("tol"))
        .def(
            "best_idx", [](const pg::population &pop) { return pop.best_idx(); },
            pygmo::population_best_idx_docstring().c_str())
        .def(
            "worst_idx",
            [](const pg::population &pop, const py::array_t<double> &tol) {
                return pop.worst_idx(pygmo::ndarr_to_vector<pg::vector_double>(tol));
            },
            py::arg("tol"))
        .def(
            "worst_idx", [](const pg::population &pop, double tol) { return pop.worst_idx(tol); }, py::arg("tol"))
        .def(
            "worst_idx", [](const pg::population &pop) { return pop.worst_idx(); },
            pygmo::population_worst_idx_docstring().c_str())
        .def("__len__", &pg::population::size)
        .def(
            "set_xf",
            [](pg::population &pop, pg::population::size_type i, const py::array_t<double> &x,
               const py::array_t<double> &f) {
                pop.set_xf(i, pygmo::ndarr_to_vector<pg::vector_double>(x),
                           pygmo::ndarr_to_vector<pg::vector_double>(f));
            },
            pygmo::population_set_xf_docstring().c_str())
        .def(
            "set_x",
            [](pg::population &pop, pg::population::size_type i, const py::array_t<double> &x) {
                pop.set_x(i, pygmo::ndarr_to_vector<pg::vector_double>(x));
            },
            pygmo::population_set_x_docstring().c_str())
        .def(
            "get_f",
            [](const pg::population &pop) { return pygmo::vvector_to_ndarr<py::array_t<double>>(pop.get_f()); },
            pygmo::population_get_f_docstring().c_str())
        .def(
            "get_x",
            [](const pg::population &pop) { return pygmo::vvector_to_ndarr<py::array_t<double>>(pop.get_x()); },
            pygmo::population_get_x_docstring().c_str())
        .def(
            "get_ID",
            [](const pg::population &pop) {
                return pygmo::vector_to_ndarr<py::array_t<unsigned long long>>(pop.get_ID());
            },
            pygmo::population_get_ID_docstring().c_str())
        .def("get_seed", &pg::population::get_seed, pygmo::population_get_seed_docstring().c_str())
        .def_property_readonly(
            "champion_x",
            [](const pg::population &pop) { return pygmo::vector_to_ndarr<py::array_t<double>>(pop.champion_x()); },
            pygmo::population_champion_x_docstring().c_str())
        .def_property_readonly(
            "champion_f",
            [](const pg::population &pop) { return pygmo::vector_to_ndarr<py::array_t<double>>(pop.champion_f()); },
            pygmo::population_champion_f_docstring().c_str())
        .def_property_readonly(
            "problem", [](pg::population &pop) -> pg::problem & { return pop.get_problem(); },
            py::return_value_policy::reference_internal, pygmo::population_problem_docstring().c_str());

    // Problem class.
    py::class_<pg::problem> problem_class(m, "problem", pygmo::problem_docstring().c_str());
    problem_class
        // Def ctor.
        .def(py::init<>())
        // repr().
        .def("__repr__", &pygmo::ostream_repr<pg::problem>)
        // Copy and deepcopy.
        .def("__copy__", &pygmo::generic_copy_wrapper<pg::problem>)
        .def("__deepcopy__", &pygmo::generic_deepcopy_wrapper<pg::problem>)
        // Pickle support.
        .def(py::pickle(&pygmo::problem_pickle_getstate, &pygmo::problem_pickle_setstate))
        // UDP extraction.
        .def("_py_extract", &pygmo::generic_py_extract<pg::problem>)
        // Problem methods.
        .def(
            "fitness",
            [](const pg::problem &p, const py::array_t<double> &dv) {
                return pygmo::vector_to_ndarr<py::array_t<double>>(
                    p.fitness(pygmo::ndarr_to_vector<pg::vector_double>(dv)));
            },
            pygmo::problem_fitness_docstring().c_str(), py::arg("dv"))
        .def(
            "get_bounds",
            [](const pg::problem &p) {
                return py::make_tuple(pygmo::vector_to_ndarr<py::array_t<double>>(p.get_lb()),
                                      pygmo::vector_to_ndarr<py::array_t<double>>(p.get_ub()));
            },
            pygmo::problem_get_bounds_docstring().c_str())
        .def(
            "get_lb", [](const pg::problem &p) { return pygmo::vector_to_ndarr<py::array_t<double>>(p.get_lb()); },
            pygmo::problem_get_lb_docstring().c_str())
        .def(
            "get_ub", [](const pg::problem &p) { return pygmo::vector_to_ndarr<py::array_t<double>>(p.get_ub()); },
            pygmo::problem_get_ub_docstring().c_str())
        .def(
            "batch_fitness",
            [](const pg::problem &p, const py::array_t<double> &dvs) {
                return pygmo::vector_to_ndarr<py::array_t<double>>(
                    p.batch_fitness(pygmo::ndarr_to_vector<pg::vector_double>(dvs)));
            },
            pygmo::problem_batch_fitness_docstring().c_str(), py::arg("dvs"))
        .def("has_batch_fitness", &pg::problem::has_batch_fitness, pygmo::problem_has_batch_fitness_docstring().c_str())
        .def(
            "gradient",
            [](const pg::problem &p, const py::array_t<double> &dv) {
                return pygmo::vector_to_ndarr<py::array_t<double>>(
                    p.gradient(pygmo::ndarr_to_vector<pg::vector_double>(dv)));
            },
            pygmo::problem_gradient_docstring().c_str(), py::arg("dv"))
        .def("has_gradient", &pg::problem::has_gradient, pygmo::problem_has_gradient_docstring().c_str())
        .def(
            "gradient_sparsity", [](const pg::problem &p) { return pygmo::sp_to_ndarr(p.gradient_sparsity()); },
            pygmo::problem_gradient_sparsity_docstring().c_str())
        .def("has_gradient_sparsity", &pg::problem::has_gradient_sparsity,
             pygmo::problem_has_gradient_sparsity_docstring().c_str())
        .def(
            "hessians",
            [](const pg::problem &p, const py::array_t<double> &dv) -> py::list {
                py::list retval;
                for (const auto &v : p.hessians(pygmo::ndarr_to_vector<pg::vector_double>(dv))) {
                    retval.append(pygmo::vector_to_ndarr<py::array_t<double>>(v));
                }
                return retval;
            },
            pygmo::problem_hessians_docstring().c_str(), py::arg("dv"))
        .def("has_hessians", &pg::problem::has_hessians, pygmo::problem_has_hessians_docstring().c_str())
        .def(
            "hessians_sparsity",
            [](const pg::problem &p) -> py::list {
                py::list retval;
                for (const auto &sp : p.hessians_sparsity()) {
                    retval.append(pygmo::sp_to_ndarr(sp));
                }
                return retval;
            },
            pygmo::problem_hessians_sparsity_docstring().c_str())
        .def("has_hessians_sparsity", &pg::problem::has_hessians_sparsity,
             pygmo::problem_has_hessians_sparsity_docstring().c_str())
        .def("get_nobj", &pg::problem::get_nobj, pygmo::problem_get_nobj_docstring().c_str())
        .def("get_nx", &pg::problem::get_nx, pygmo::problem_get_nx_docstring().c_str())
        .def("get_nix", &pg::problem::get_nix, pygmo::problem_get_nix_docstring().c_str())
        .def("get_ncx", &pg::problem::get_ncx, pygmo::problem_get_ncx_docstring().c_str())
        .def("get_nf", &pg::problem::get_nf, pygmo::problem_get_nf_docstring().c_str())
        .def("get_nec", &pg::problem::get_nec, pygmo::problem_get_nec_docstring().c_str())
        .def("get_nic", &pg::problem::get_nic, pygmo::problem_get_nic_docstring().c_str())
        .def("get_nc", &pg::problem::get_nc, pygmo::problem_get_nc_docstring().c_str())
        .def("get_fevals", &pg::problem::get_fevals, pygmo::problem_get_fevals_docstring().c_str())
        .def("increment_fevals", &pg::problem::increment_fevals, pygmo::problem_increment_fevals_docstring().c_str(),
             py::arg("n"))
        .def("get_gevals", &pg::problem::get_gevals, pygmo::problem_get_gevals_docstring().c_str())
        .def("get_hevals", &pg::problem::get_hevals, pygmo::problem_get_hevals_docstring().c_str())
        .def("set_seed", &pg::problem::set_seed, pygmo::problem_set_seed_docstring().c_str(), py::arg("seed"))
        .def("has_set_seed", &pg::problem::has_set_seed, pygmo::problem_has_set_seed_docstring().c_str())
        .def("is_stochastic", &pg::problem::is_stochastic,
             "is_stochastic()\n\nAlias for :func:`~pygmo.problem.has_set_seed()`.\n")
        .def(
            "feasibility_x",
            [](const pg::problem &p, const py::array_t<double> &x) {
                return p.feasibility_x(pygmo::ndarr_to_vector<pg::vector_double>(x));
            },
            pygmo::problem_feasibility_x_docstring().c_str(), py::arg("x"))
        .def(
            "feasibility_f",
            [](const pg::problem &p, const py::array_t<double> &f) {
                return p.feasibility_f(pygmo::ndarr_to_vector<pg::vector_double>(f));
            },
            pygmo::problem_feasibility_f_docstring().c_str(), py::arg("f"))
        .def("get_name", &pg::problem::get_name, pygmo::problem_get_name_docstring().c_str())
        .def("get_extra_info", &pg::problem::get_extra_info, pygmo::problem_get_extra_info_docstring().c_str())
        .def("get_thread_safety", &pg::problem::get_thread_safety, pygmo::problem_get_thread_safety_docstring().c_str())
        .def_property(
            "c_tol",
            [](const pg::problem &prob) { return pygmo::vector_to_ndarr<py::array_t<double>>(prob.get_c_tol()); },
            [](pg::problem &prob, const py::object &c_tol) {
                try {
                    prob.set_c_tol(py::cast<double>(c_tol));
                } catch (const py::cast_error &) {
                    prob.set_c_tol(pygmo::ndarr_to_vector<pg::vector_double>(py::cast<py::array_t<double>>(c_tol)));
                }
            },
            pygmo::problem_c_tol_docstring().c_str());

    // Expose the C++ problems.
    pygmo::expose_problems_0(m, problem_class, problems_module);
    pygmo::expose_problems_1(m, problem_class, problems_module);

    // Finalize.
    problem_class.def(py::init<const py::object &>(), py::arg("udp"));

    // Algorithm class.
    py::class_<pg::algorithm> algorithm_class(m, "algorithm", pygmo::algorithm_docstring().c_str());
    algorithm_class
        // Def ctor.
        .def(py::init<>())
        // repr().
        .def("__repr__", &pygmo::ostream_repr<pg::algorithm>)
        // Copy and deepcopy.
        .def("__copy__", &pygmo::generic_copy_wrapper<pg::algorithm>)
        .def("__deepcopy__", &pygmo::generic_deepcopy_wrapper<pg::algorithm>)
        // Pickle support.
        .def(py::pickle(&pygmo::algorithm_pickle_getstate, &pygmo::algorithm_pickle_setstate))
        // UDA extraction.
        .def("_py_extract", &pygmo::generic_py_extract<pg::algorithm>)
        // Algorithm methods.
        .def("evolve", &pg::algorithm::evolve, pygmo::algorithm_evolve_docstring().c_str(), py::arg("pop"))
        .def("set_seed", &pg::algorithm::set_seed, pygmo::algorithm_set_seed_docstring().c_str(), py::arg("seed"))
        .def("has_set_seed", &pg::algorithm::has_set_seed, pygmo::algorithm_has_set_seed_docstring().c_str())
        .def("set_verbosity", &pg::algorithm::set_verbosity, pygmo::algorithm_set_verbosity_docstring().c_str(),
             py::arg("level"))
        .def("has_set_verbosity", &pg::algorithm::has_set_verbosity,
             pygmo::algorithm_has_set_verbosity_docstring().c_str())
        .def("is_stochastic", &pg::algorithm::is_stochastic,
             "is_stochastic()\n\nAlias for :func:`~pygmo.algorithm.has_set_seed()`.\n")
        .def("get_name", &pg::algorithm::get_name, pygmo::algorithm_get_name_docstring().c_str())
        .def("get_extra_info", &pg::algorithm::get_extra_info, pygmo::algorithm_get_extra_info_docstring().c_str())
        .def("get_thread_safety", &pg::algorithm::get_thread_safety,
             pygmo::algorithm_get_thread_safety_docstring().c_str());

    // Expose the C++ algos.
    pygmo::expose_algorithms_0(m, algorithm_class, algorithms_module);
    pygmo::expose_algorithms_1(m, algorithm_class, algorithms_module);

    // Finalize.
    algorithm_class.def(py::init<const py::object &>(), py::arg("uda"));

    // bfe class.
    py::class_<pg::bfe> bfe_class(m, "bfe", pygmo::bfe_docstring().c_str());
    bfe_class
        // Def ctor.
        .def(py::init<>())
        // repr().
        .def("__repr__", &pygmo::ostream_repr<pg::bfe>)
        // Copy and deepcopy.
        .def("__copy__", &pygmo::generic_copy_wrapper<pg::bfe>)
        .def("__deepcopy__", &pygmo::generic_deepcopy_wrapper<pg::bfe>)
        // Pickle support.
        .def(py::pickle(&pygmo::bfe_pickle_getstate, &pygmo::bfe_pickle_setstate))
        // UDBFE extraction.
        .def("_py_extract", &pygmo::generic_py_extract<pg::bfe>)
        // Bfe methods.
        .def("_call_impl",
             [](const pg::bfe &b, const pg::problem &prob, const py::array_t<double> &dvs) {
                 return pygmo::vector_to_ndarr<py::array_t<double>>(
                     b(prob, pygmo::ndarr_to_vector<pg::vector_double>(dvs)));
             })
        .def("get_name", &pg::bfe::get_name, pygmo::bfe_get_name_docstring().c_str())
        .def("get_extra_info", &pg::bfe::get_extra_info, pygmo::bfe_get_extra_info_docstring().c_str())
        .def("get_thread_safety", &pg::bfe::get_thread_safety, pygmo::bfe_get_thread_safety_docstring().c_str());

    // Expose the C++ bfes.
    pygmo::expose_bfes(m, bfe_class, batch_evaluators_module);

    // Finalize.
    bfe_class.def(py::init<const py::object &>(), py::arg("udbfe"));

    // Island class.
    py::class_<pg::island> island_class(m, "island", pygmo::island_docstring().c_str());
    island_class
        // Def ctor.
        .def(py::init<>())
        // Ctor from algo, pop, and policies.
        .def(py::init<const pg::algorithm &, const pg::population &, const pg::r_policy &, const pg::s_policy &>())
        // repr().
        .def("__repr__", &pygmo::ostream_repr<pg::island>)
        // Copy and deepcopy.
        .def("__copy__", &pygmo::generic_copy_wrapper<pg::island>)
        .def("__deepcopy__", &pygmo::generic_deepcopy_wrapper<pg::island>)
        // Pickle support.
        .def(py::pickle(&pygmo::island_pickle_getstate, &pygmo::island_pickle_setstate))
        // UDI extraction.
        .def("_py_extract", &pygmo::generic_py_extract<pg::island>)
        .def(
            "evolve", [](pg::island &isl, unsigned n) { isl.evolve(n); }, pygmo::island_evolve_docstring().c_str(),
            py::arg("n") = 1u)
        .def("wait", &pg::island::wait, pygmo::island_wait_docstring().c_str())
        .def("wait_check", &pg::island::wait_check, pygmo::island_wait_check_docstring().c_str())
        .def("get_population", &pg::island::get_population, pygmo::island_get_population_docstring().c_str())
        .def("get_algorithm", &pg::island::get_algorithm, pygmo::island_get_algorithm_docstring().c_str())
        .def("set_population", &pg::island::set_population, pygmo::island_set_population_docstring().c_str(),
             py::arg("pop"))
        .def("set_algorithm", &pg::island::set_algorithm, pygmo::island_set_algorithm_docstring().c_str(),
             py::arg("algo"))
        .def("get_name", &pg::island::get_name, pygmo::island_get_name_docstring().c_str())
        .def("get_extra_info", &pg::island::get_extra_info, pygmo::island_get_extra_info_docstring().c_str())
        .def("get_r_policy", &pg::island::get_r_policy, pygmo::island_get_r_policy_docstring().c_str())
        .def("get_s_policy", &pg::island::get_s_policy, pygmo::island_get_s_policy_docstring().c_str())
        .def_property_readonly("status", &pg::island::status, pygmo::island_status_docstring().c_str());

    // Expose the C++ islands.
    pygmo::expose_islands(m, island_class, islands_module);

    // Finalize.
    island_class.def(py::init<const py::object &, const pg::algorithm &, const pg::population &, const pg::r_policy &,
                              const pg::s_policy &>());

    // Replacement policy class.
    py::class_<pg::r_policy> r_policy_class(m, "r_policy", pygmo::r_policy_docstring().c_str());
    r_policy_class
        // Def ctor.
        .def(py::init<>())
        // repr().
        .def("__repr__", &pygmo::ostream_repr<pg::r_policy>)
        // Copy and deepcopy.
        .def("__copy__", &pygmo::generic_copy_wrapper<pg::r_policy>)
        .def("__deepcopy__", &pygmo::generic_deepcopy_wrapper<pg::r_policy>)
        // Pickle support.
        .def(py::pickle(&pygmo::r_policy_pickle_getstate, &pygmo::r_policy_pickle_setstate))
        // UDRP extraction.
        .def("_py_extract", &pygmo::generic_py_extract<pg::r_policy>)
        // r_policy methods.
        .def(
            "replace",
            [](const pg::r_policy &r, const py::iterable &inds, const pg::vector_double::size_type &nx,
               const pg::vector_double::size_type &nix, const pg::vector_double::size_type &nobj,
               const pg::vector_double::size_type &nec, const pg::vector_double::size_type &nic,
               const py::array_t<double> &tol, const py::iterable &mig) {
                return pygmo::inds_to_tuple(r.replace(pygmo::iterable_to_inds(inds), nx, nix, nobj, nec, nic,
                                                      pygmo::ndarr_to_vector<pg::vector_double>(tol),
                                                      pygmo::iterable_to_inds(mig)));
            },
            pygmo::r_policy_replace_docstring().c_str(), py::arg("inds"), py::arg("nx"), py::arg("nix"),
            py::arg("nobj"), py::arg("nec"), py::arg("nic"), py::arg("tol"), py::arg("mig"))
        .def("get_name", &pg::r_policy::get_name, pygmo::r_policy_get_name_docstring().c_str())
        .def("get_extra_info", &pg::r_policy::get_extra_info, pygmo::r_policy_get_extra_info_docstring().c_str());

    // Expose the C++ replacement policies.
    pygmo::expose_r_policies(m, r_policy_class, r_policies_module);

    // Finalize.
    r_policy_class.def(py::init<const py::object &>(), py::arg("udrp"));
}