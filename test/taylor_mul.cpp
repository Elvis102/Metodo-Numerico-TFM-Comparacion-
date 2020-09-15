// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <algorithm>
#include <initializer_list>
#include <random>
#include <tuple>
#include <vector>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/binary_operator.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/taylor.hpp>

#include "catch.hpp"
#include "test_utils.hpp"

static std::mt19937 rng;

using namespace heyoka;
using namespace heyoka_test;

const auto fp_types = std::tuple<double, long double
#if defined(HEYOKA_HAVE_REAL128)
                                 ,
                                 mppp::real128
#endif
                                 >{};

template <typename T, typename U>
void compare_batch_scalar(std::initializer_list<U> sys, unsigned opt_level, bool high_accuracy)
{
    const auto batch_size = 23u;

    llvm_state s{kw::opt_level = opt_level};

    taylor_add_jet<T>(s, "jet_batch", sys, 3, batch_size, high_accuracy);
    taylor_add_jet<T>(s, "jet_scalar", sys, 3, 1, high_accuracy);

    s.compile();

    auto jptr_batch = reinterpret_cast<void (*)(T *)>(s.jit_lookup("jet_batch"));
    auto jptr_scalar = reinterpret_cast<void (*)(T *)>(s.jit_lookup("jet_scalar"));

    std::vector<T> jet_batch;
    jet_batch.resize(8 * batch_size);
    std::uniform_real_distribution<float> dist(-10.f, 10.f);
    std::generate(jet_batch.begin(), jet_batch.end(), [&dist]() { return T{dist(rng)}; });

    std::vector<T> jet_scalar;
    jet_scalar.resize(8);

    jptr_batch(jet_batch.data());

    for (auto batch_idx = 0u; batch_idx < batch_size; ++batch_idx) {
        // Assign the initial values of x and y.
        for (auto i = 0u; i < 2u; ++i) {
            jet_scalar[i] = jet_batch[i * batch_size + batch_idx];
        }

        jptr_scalar(jet_scalar.data());

        for (auto i = 2u; i < 8u; ++i) {
            REQUIRE(jet_scalar[i] == approximately(jet_batch[i * batch_size + batch_idx]));
        }
    }
}

TEST_CASE("taylor mul")
{
    auto tester = [](auto fp_x, unsigned opt_level, bool high_accuracy) {
        using fp_t = decltype(fp_x);

        using Catch::Matchers::Message;

        auto x = "x"_var, y = "y"_var;

        // Number-number tests.
        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet",
                                 {expression{binary_operator{binary_operator::type::mul, 2_dbl, 3_dbl}}, x + y}, 1, 1,
                                 high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(fp_t{6}));
            REQUIRE(jet[3] == approximately(fp_t{5}));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet",
                                 {expression{binary_operator{binary_operator::type::mul, 2_dbl, 3_dbl}}, x + y}, 1, 2,
                                 high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-2}, fp_t{3}, fp_t{-3}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -2);
            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -3);
            REQUIRE(jet[4] == 6);
            REQUIRE(jet[5] == 6);
            REQUIRE(jet[6] == 5);
            REQUIRE(jet[7] == -5);
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet",
                                 {expression{binary_operator{binary_operator::type::mul, 2_dbl, 3_dbl}}, x + y}, 2, 1,
                                 high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(fp_t{6}));
            REQUIRE(jet[3] == approximately(fp_t{5}));
            REQUIRE(jet[4] == 0);
            REQUIRE(jet[5] == approximately(fp_t{1} / 2 * (fp_t{6} + jet[3])));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet",
                                 {expression{binary_operator{binary_operator::type::mul, 2_dbl, 3_dbl}}, x + y}, 2, 2,
                                 high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-2}, fp_t{3}, fp_t{-3}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -2);
            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -3);
            REQUIRE(jet[4] == approximately(fp_t{6}));
            REQUIRE(jet[5] == approximately(fp_t{6}));
            REQUIRE(jet[6] == approximately(fp_t{5}));
            REQUIRE(jet[7] == approximately(-fp_t{5}));
            REQUIRE(jet[8] == 0);
            REQUIRE(jet[9] == 0);
            REQUIRE(jet[10] == approximately(.5 * (fp_t{6} + jet[6])));
            REQUIRE(jet[11] == approximately(.5 * (fp_t{6} + jet[7])));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet",
                                 {expression{binary_operator{binary_operator::type::mul, 2_dbl, 3_dbl}}, x + y}, 3, 3,
                                 high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-2}, fp_t{-1}, fp_t{3}, fp_t{2}, fp_t{4}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -2);
            REQUIRE(jet[2] == -1);
            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == 2);
            REQUIRE(jet[5] == 4);
            REQUIRE(jet[6] == approximately(fp_t{6}));
            REQUIRE(jet[7] == approximately(fp_t{6}));
            REQUIRE(jet[8] == approximately(fp_t{6}));
            REQUIRE(jet[9] == approximately(fp_t{5}));
            REQUIRE(jet[10] == approximately(fp_t{0}));
            REQUIRE(jet[11] == approximately(fp_t{3}));
            REQUIRE(jet[12] == 0);
            REQUIRE(jet[13] == 0);
            REQUIRE(jet[14] == 0);
            REQUIRE(jet[15] == approximately(fp_t{1} / 2 * (fp_t{6} + jet[9])));
            REQUIRE(jet[16] == approximately(fp_t{1} / 2 * (fp_t{6} + jet[10])));
            REQUIRE(jet[17] == approximately(fp_t{1} / 2 * (fp_t{6} + jet[11])));
            REQUIRE(jet[18] == 0);
            REQUIRE(jet[19] == 0);
            REQUIRE(jet[20] == 0);
            REQUIRE(jet[21] == approximately(1 / fp_t{6} * (2 * jet[15])));
            REQUIRE(jet[22] == approximately(1 / fp_t{6} * (2 * jet[16])));
            REQUIRE(jet[23] == approximately(1 / fp_t{6} * (2 * jet[17])));
        }

        // Do the batch/scalar comparison.
        compare_batch_scalar<fp_t>({expression{binary_operator{binary_operator::type::mul, 2_dbl, 3_dbl}}, x + y},
                                   opt_level, high_accuracy);

        // Variable-number tests.
        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {y * 2_dbl, x * -4_dbl}, 1, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(fp_t{6}));
            REQUIRE(jet[3] == approximately(-fp_t{8}));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {y * 2_dbl, x * -4_dbl}, 1, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{1}, fp_t{3}, fp_t{-4}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 1);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -4);

            REQUIRE(jet[4] == approximately(fp_t{6}));
            REQUIRE(jet[5] == approximately(-fp_t{8}));

            REQUIRE(jet[6] == approximately(-fp_t{8}));
            REQUIRE(jet[7] == approximately(-fp_t{4}));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {y * 2_dbl, x * -4_dbl}, 2, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(fp_t{6}));
            REQUIRE(jet[3] == approximately(-fp_t{8}));
            REQUIRE(jet[4] == approximately(jet[3]));
            REQUIRE(jet[5] == approximately(-2 * jet[2]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {y * 2_dbl, x * -4_dbl}, 2, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{3}, fp_t{4}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);
            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 4);
            REQUIRE(jet[4] == approximately(fp_t{6}));
            REQUIRE(jet[5] == approximately(fp_t{8}));
            REQUIRE(jet[6] == approximately(-fp_t{8}));
            REQUIRE(jet[7] == approximately(fp_t{4}));
            REQUIRE(jet[8] == approximately(jet[6]));
            REQUIRE(jet[9] == approximately(jet[7]));
            REQUIRE(jet[10] == approximately(-2 * jet[4]));
            REQUIRE(jet[11] == approximately(-2 * jet[5]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {y * 2_dbl, x * -4_dbl}, 3, 3, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{0}, fp_t{3}, fp_t{4}, fp_t{-5}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);
            REQUIRE(jet[2] == 0);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == 4);
            REQUIRE(jet[5] == -5);

            REQUIRE(jet[6] == approximately(fp_t{6}));
            REQUIRE(jet[7] == approximately(fp_t{8}));
            REQUIRE(jet[8] == approximately(fp_t{-10}));

            REQUIRE(jet[9] == approximately(-fp_t{8}));
            REQUIRE(jet[10] == approximately(fp_t{4}));
            REQUIRE(jet[11] == approximately(fp_t{0}));

            REQUIRE(jet[12] == approximately(jet[9]));
            REQUIRE(jet[13] == approximately(jet[10]));
            REQUIRE(jet[14] == approximately(jet[11]));

            REQUIRE(jet[15] == approximately(-2 * jet[6]));
            REQUIRE(jet[16] == approximately(-2 * jet[7]));
            REQUIRE(jet[17] == approximately(-2 * jet[8]));

            REQUIRE(jet[18] == approximately(1 / fp_t{6} * 4 * jet[15]));
            REQUIRE(jet[19] == approximately(1 / fp_t{6} * 4 * jet[16]));
            REQUIRE(jet[20] == approximately(1 / fp_t{6} * 4 * jet[17]));

            REQUIRE(jet[21] == approximately(-1 / fp_t{6} * 8 * jet[12]));
            REQUIRE(jet[22] == approximately(-1 / fp_t{6} * 8 * jet[13]));
            REQUIRE(jet[23] == approximately(-1 / fp_t{6} * 8 * jet[14]));
        }

        compare_batch_scalar<fp_t>({y * 2_dbl, x * -4_dbl}, opt_level, high_accuracy);

        // Number/variable tests.
        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {2_dbl * y, -4_dbl * x}, 1, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(fp_t{6}));
            REQUIRE(jet[3] == approximately(-fp_t{8}));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {2_dbl * y, -4_dbl * x}, 1, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{3}, fp_t{4}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 4);

            REQUIRE(jet[4] == approximately(fp_t{6}));
            REQUIRE(jet[5] == approximately(fp_t{8}));

            REQUIRE(jet[6] == approximately(-fp_t{8}));
            REQUIRE(jet[7] == approximately(fp_t{4}));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {2_dbl * y, -4_dbl * x}, 2, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(fp_t{6}));
            REQUIRE(jet[3] == approximately(-fp_t{8}));
            REQUIRE(jet[4] == approximately(jet[3]));
            REQUIRE(jet[5] == approximately(-2 * jet[2]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {2_dbl * y, -4_dbl * x}, 2, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{3}, fp_t{4}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);
            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 4);
            REQUIRE(jet[4] == approximately(fp_t{6}));
            REQUIRE(jet[5] == approximately(fp_t{8}));
            REQUIRE(jet[6] == approximately(-fp_t{8}));
            REQUIRE(jet[7] == approximately(fp_t{4}));
            REQUIRE(jet[8] == approximately(jet[6]));
            REQUIRE(jet[9] == approximately(jet[7]));
            REQUIRE(jet[10] == approximately(-2 * jet[4]));
            REQUIRE(jet[11] == approximately(-2 * jet[5]));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {2_dbl * y, -4_dbl * x}, 3, 3, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{0}, fp_t{3}, fp_t{4}, fp_t{-5}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);
            REQUIRE(jet[2] == 0);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == 4);
            REQUIRE(jet[5] == -5);

            REQUIRE(jet[6] == approximately(fp_t{6}));
            REQUIRE(jet[7] == approximately(fp_t{8}));
            REQUIRE(jet[8] == approximately(fp_t{-10}));

            REQUIRE(jet[9] == approximately(-fp_t{8}));
            REQUIRE(jet[10] == approximately(fp_t{4}));
            REQUIRE(jet[11] == approximately(fp_t{0}));

            REQUIRE(jet[12] == approximately(jet[9]));
            REQUIRE(jet[13] == approximately(jet[10]));
            REQUIRE(jet[14] == approximately(jet[11]));

            REQUIRE(jet[15] == approximately(-2 * jet[6]));
            REQUIRE(jet[16] == approximately(-2 * jet[7]));
            REQUIRE(jet[17] == approximately(-2 * jet[8]));

            REQUIRE(jet[18] == approximately(1 / fp_t{6} * 4 * jet[15]));
            REQUIRE(jet[19] == approximately(1 / fp_t{6} * 4 * jet[16]));
            REQUIRE(jet[20] == approximately(1 / fp_t{6} * 4 * jet[17]));

            REQUIRE(jet[21] == approximately(-1 / fp_t{6} * 8 * jet[12]));
            REQUIRE(jet[22] == approximately(-1 / fp_t{6} * 8 * jet[13]));
            REQUIRE(jet[23] == approximately(-1 / fp_t{6} * 8 * jet[14]));
        }

        compare_batch_scalar<fp_t>({2_dbl * y, -4_dbl * x}, opt_level, high_accuracy);

        // Variable/variable tests.
        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {x * y, y * x}, 1, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(fp_t{6}));
            REQUIRE(jet[3] == approximately(fp_t{6}));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {x * y, y * x}, 1, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{1}, fp_t{3}, fp_t{-4}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 1);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -4);

            REQUIRE(jet[4] == approximately(fp_t{6}));
            REQUIRE(jet[5] == approximately(-fp_t{4}));

            REQUIRE(jet[6] == approximately(fp_t{6}));
            REQUIRE(jet[7] == approximately(-fp_t{4}));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {x * y, y * x}, 2, 1, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(fp_t{6}));
            REQUIRE(jet[3] == approximately(fp_t{6}));
            REQUIRE(jet[4] == approximately(fp_t{1} / 2 * (jet[2] * 3 + jet[3] * 2)));
            REQUIRE(jet[5] == approximately(fp_t{1} / 2 * (jet[2] * 3 + jet[3] * 2)));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {x * y, y * x}, 2, 2, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{1}, fp_t{3}, fp_t{-4}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 1);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -4);

            REQUIRE(jet[4] == approximately(fp_t{6}));
            REQUIRE(jet[5] == approximately(-fp_t{4}));

            REQUIRE(jet[6] == approximately(fp_t{6}));
            REQUIRE(jet[7] == approximately(-fp_t{4}));

            REQUIRE(jet[8] == approximately(fp_t{1} / 2 * (jet[4] * 3 + jet[6] * 2)));
            REQUIRE(jet[9] == approximately(fp_t{1} / 2 * (jet[5] * -4 + jet[7] * 1)));

            REQUIRE(jet[10] == approximately(fp_t{1} / 2 * (jet[4] * 3 + jet[6] * 2)));
            REQUIRE(jet[11] == approximately(fp_t{1} / 2 * (jet[5] * -4 + jet[7] * 1)));
        }

        {
            llvm_state s{kw::opt_level = opt_level};

            taylor_add_jet<fp_t>(s, "jet", {x * y, y * x}, 3, 3, high_accuracy);

            s.compile();

            auto jptr = reinterpret_cast<void (*)(fp_t *)>(s.jit_lookup("jet"));

            std::vector<fp_t> jet{fp_t{2}, fp_t{1}, fp_t{3}, fp_t{3}, fp_t{-4}, fp_t{6}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 1);
            REQUIRE(jet[2] == 3);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == -4);
            REQUIRE(jet[5] == 6);

            REQUIRE(jet[6] == approximately(fp_t{6}));
            REQUIRE(jet[7] == approximately(-fp_t{4}));
            REQUIRE(jet[8] == approximately(fp_t{18}));

            REQUIRE(jet[9] == approximately(fp_t{6}));
            REQUIRE(jet[10] == approximately(-fp_t{4}));
            REQUIRE(jet[11] == approximately(fp_t{18}));

            REQUIRE(jet[12] == approximately(fp_t{1} / 2 * (jet[6] * 3 + jet[9] * 2)));
            REQUIRE(jet[13] == approximately(fp_t{1} / 2 * (jet[7] * -4 + jet[10] * 1)));
            REQUIRE(jet[14] == approximately(fp_t{1} / 2 * (jet[8] * 6 + jet[11] * 3)));

            REQUIRE(jet[15] == approximately(fp_t{1} / 2 * (jet[6] * 3 + jet[9] * 2)));
            REQUIRE(jet[16] == approximately(fp_t{1} / 2 * (jet[7] * -4 + jet[10] * 1)));
            REQUIRE(jet[17] == approximately(fp_t{1} / 2 * (jet[8] * 6 + jet[11] * 3)));

            REQUIRE(jet[18] == approximately(1 / fp_t{6} * (2 * jet[12] * 3 + 2 * jet[6] * jet[9] + 2 * 2 * jet[15])));
            REQUIRE(jet[19]
                    == approximately(1 / fp_t{6} * (2 * jet[13] * -4 + 2 * jet[7] * jet[10] + 2 * 1 * jet[16])));
            REQUIRE(jet[20] == approximately(1 / fp_t{6} * (2 * jet[14] * 6 + 2 * jet[8] * jet[11] + 2 * 3 * jet[17])));

            REQUIRE(jet[21] == approximately(1 / fp_t{6} * (2 * jet[12] * 3 + 2 * jet[6] * jet[9] + 2 * 2 * jet[15])));
            REQUIRE(jet[22]
                    == approximately(1 / fp_t{6} * (2 * jet[13] * -4 + 2 * jet[7] * jet[10] + 2 * 1 * jet[16])));
            REQUIRE(jet[23] == approximately(1 / fp_t{6} * (2 * jet[14] * 6 + 2 * jet[8] * jet[11] + 2 * 3 * jet[17])));
        }

        compare_batch_scalar<fp_t>({x * y, y * x}, opt_level, high_accuracy);
    };

    for (auto f : {false, true}) {
        tuple_for_each(fp_types, [&tester, f](auto x) { tester(x, 0, f); });
        tuple_for_each(fp_types, [&tester, f](auto x) { tester(x, 1, f); });
        tuple_for_each(fp_types, [&tester, f](auto x) { tester(x, 2, f); });
        tuple_for_each(fp_types, [&tester, f](auto x) { tester(x, 3, f); });
    }
}
