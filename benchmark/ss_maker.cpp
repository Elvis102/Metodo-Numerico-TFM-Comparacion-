// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/program_options.hpp>

#include <xtensor/xadapt.hpp>
#include <xtensor/xio.hpp>
#include <xtensor/xmath.hpp>
#include <xtensor/xview.hpp>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/nbody.hpp>
#include <heyoka/taylor.hpp>

using namespace heyoka;

template <typename T>
void run_bench(std::uint32_t nplanets, T tol, bool high_accuracy, bool compact_mode, bool fast_math)
{
    // Init the masses vector with the solar mass.
    std::vector masses{T(1)};

    // Add the Earth-like planets' masses.
    for (std::uint32_t i = 0; i < nplanets; ++i) {
        masses.push_back(T(1) / 333000);
    }

    // G constant, in terms of solar masses, AUs and years.
    const auto G = T(0.01720209895) * T(0.01720209895) * 365 * 365;

    // Create the nbody system.
    auto sys = make_nbody_sys(nplanets + 1u, kw::masses = masses, kw::Gconst = G);

    // The initial state (zeroed out, change it later).
    std::vector<T> init_state((nplanets + 1u) * 6u);

    auto start = std::chrono::high_resolution_clock::now();

    // Create the integrator.
    taylor_adaptive<T> ta{
        std::move(sys), std::move(init_state),    kw::high_accuracy = high_accuracy, kw::compact_mode = compact_mode,
        kw::tol = tol,  kw::fast_math = fast_math};

    auto elapsed = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start)
            .count());

    std::cout << "Construction time: " << elapsed << "ms\n";

    // Create an xtensor view on the the state vector
    // for ease of indexing.
    auto s_array = xt::adapt(ta.get_state_data(), {nplanets + 1u, 6u});

    // Set the initial positions at regular intervals on the x axis
    // on circular orbits. The Sun is already in the origin with zero
    // velocity.
    for (std::uint32_t i = 0; i < nplanets; ++i) {
        using std::sqrt;

        s_array(i + 1u, 0) = i + 1u;
        s_array(i + 1u, 4) = sqrt(G / (i + 1u));
    }

    // Adjust the Sun's velocity so that the COM of the system does not move.
    s_array(0, 4) = -T(1) / 333000 * xt::sum(xt::view(s_array, xt::range(1, xt::placeholders::_), 4))[0];

    start = std::chrono::high_resolution_clock::now();

    // Run the integration for 1e5 years.
    ta.propagate_until(T(1e5));

    elapsed = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start)
            .count());

    std::cout << "Integration time: " << elapsed << "ms\n";

    std::cout << s_array << '\n';
}

int main(int argc, char *argv[])
{
    namespace po = boost::program_options;

    std::string fp_type;
    double tol;
    bool compact_mode = false;
    bool high_accuracy = false;
    bool fast_math = false;
    std::uint32_t nplanets;

    po::options_description desc("Options");

    desc.add_options()("help", "produce help message")(
        "fp_type", po::value<std::string>(&fp_type)->default_value("double"), "floating-point type")(
        "tol", po::value<double>(&tol)->default_value(0.), "tolerance (if 0, it will be the type's epsilon)")(
        "high_accuracy", "enable high-accuracy mode")("compact_mode", "enable compact mode")(
        "fast_math", "enable fast math flags")("nplanets", po::value<std::uint32_t>(&nplanets)->default_value(1),
                                               "number of planets (>=1)");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }

    if (nplanets == 0u) {
        throw std::invalid_argument("The number of planets cannot be zero");
    }

    if (vm.count("high_accuracy")) {
        high_accuracy = true;
    }

    if (vm.count("compact_mode")) {
        compact_mode = true;
    }

    if (vm.count("fast_math")) {
        fast_math = true;
    }

    if (fp_type == "double") {
        run_bench<double>(nplanets, tol, high_accuracy, compact_mode, fast_math);
    } else if (fp_type == "long double") {
        run_bench<long double>(nplanets, tol, high_accuracy, compact_mode, fast_math);
#if defined(HEYOKA_HAVE_REAL128)
    } else if (fp_type == "real128") {
        run_bench<mppp::real128>(nplanets, mppp::real128(tol), high_accuracy, compact_mode, fast_math);
#endif
    } else {
        throw std::invalid_argument("Invalid floating-point type: '" + fp_type + "'");
    }
}
