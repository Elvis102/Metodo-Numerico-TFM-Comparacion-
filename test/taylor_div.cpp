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
void compare_batch_scalar(std::initializer_list<U> sys)
{
    const auto batch_size = 23u;

    llvm_state s{"", 0};

    s.add_taylor_jet_batch<T>("jet_batch", sys, 3, batch_size);
    s.add_taylor_jet_batch<T>("jet_scalar", sys, 3, 1);

    s.compile();

    auto jptr_batch = s.fetch_taylor_jet_batch<T>("jet_batch");
    auto jptr_scalar = s.fetch_taylor_jet_batch<T>("jet_scalar");

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

TEST_CASE("taylor div")
{
    auto tester = [](auto fp_x) {
        using fp_t = decltype(fp_x);

        using Catch::Matchers::Message;

        auto x = "x"_var, y = "y"_var;

        // Number-number tests.
        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>(
                "jet", {expression{binary_operator{binary_operator::type::div, 1_dbl, 3_dbl}}, x + y}, 1, 1);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(1 / fp_t{3}));
            REQUIRE(jet[3] == approximately(fp_t{5}));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>(
                "jet", {expression{binary_operator{binary_operator::type::div, 1_dbl, 3_dbl}}, x + y}, 2, 1);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(1 / fp_t{3}));
            REQUIRE(jet[3] == approximately(fp_t{5}));
            REQUIRE(jet[4] == 0);
            REQUIRE(jet[5] == approximately(fp_t{1} / 2 * (jet[2] + jet[3])));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>(
                "jet", {expression{binary_operator{binary_operator::type::div, 1_dbl, 3_dbl}}, x + y}, 1, 2);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{3}, fp_t{5}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 5);

            REQUIRE(jet[4] == approximately(1 / fp_t{3}));
            REQUIRE(jet[5] == approximately(1 / fp_t{3}));

            REQUIRE(jet[6] == approximately(fp_t{5}));
            REQUIRE(jet[7] == approximately(fp_t{4}));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>(
                "jet", {expression{binary_operator{binary_operator::type::div, 1_dbl, 3_dbl}}, x + y}, 2, 2);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{-1}, fp_t{3}, fp_t{5}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -1);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 5);

            REQUIRE(jet[4] == approximately(1 / fp_t{3}));
            REQUIRE(jet[5] == approximately(1 / fp_t{3}));

            REQUIRE(jet[6] == approximately(fp_t{5}));
            REQUIRE(jet[7] == approximately(fp_t{4}));

            REQUIRE(jet[8] == 0);
            REQUIRE(jet[9] == 0);

            REQUIRE(jet[10] == approximately(fp_t{1} / 2 * (jet[4] + jet[6])));
            REQUIRE(jet[11] == approximately(fp_t{1} / 2 * (jet[5] + jet[7])));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>(
                "jet", {expression{binary_operator{binary_operator::type::div, 1_dbl, 3_dbl}}, x + y}, 3, 3);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{1}, fp_t{-6}, fp_t{3}, fp_t{-4}, fp_t{2}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 1);
            REQUIRE(jet[2] == -6);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == -4);
            REQUIRE(jet[5] == 2);

            REQUIRE(jet[6] == approximately(1 / fp_t{3}));
            REQUIRE(jet[7] == approximately(1 / fp_t{3}));
            REQUIRE(jet[8] == approximately(1 / fp_t{3}));

            REQUIRE(jet[9] == approximately(fp_t{5}));
            REQUIRE(jet[10] == approximately(fp_t{-3}));
            REQUIRE(jet[11] == approximately(fp_t{-4}));

            REQUIRE(jet[12] == 0);
            REQUIRE(jet[13] == 0);
            REQUIRE(jet[14] == 0);

            REQUIRE(jet[15] == approximately(fp_t{1} / 2 * (1 / fp_t{3} + jet[9])));
            REQUIRE(jet[16] == approximately(fp_t{1} / 2 * (1 / fp_t{3} + jet[10])));
            REQUIRE(jet[17] == approximately(fp_t{1} / 2 * (1 / fp_t{3} + jet[11])));

            REQUIRE(jet[18] == 0);
            REQUIRE(jet[19] == 0);
            REQUIRE(jet[20] == 0);

            REQUIRE(jet[21] == approximately(1 / fp_t{6} * (2 * jet[15])));
            REQUIRE(jet[22] == approximately(1 / fp_t{6} * (2 * jet[16])));
            REQUIRE(jet[23] == approximately(1 / fp_t{6} * (2 * jet[17])));
        }

        // Do the batch/scalar comparison.
        compare_batch_scalar<fp_t>({expression{binary_operator{binary_operator::type::div, 1_dbl, 3_dbl}}, x + y});

        // Variable-number tests.
        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {y / 2_dbl, x / -4_dbl}, 1, 1);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(3 / fp_t{2}));
            REQUIRE(jet[3] == approximately(2 / fp_t{-4}));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {y / 2_dbl, x / -4_dbl}, 2, 1);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(3 / fp_t{2}));
            REQUIRE(jet[3] == approximately(2 / fp_t{-4}));
            REQUIRE(jet[4] == approximately(fp_t{1} / 2 * (jet[3] / 2)));
            REQUIRE(jet[5] == approximately(fp_t{1} / 2 * (jet[2] / -4)));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {y / 2_dbl, x / -4_dbl}, 1, 2);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{1}, fp_t{3}, fp_t{-4}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 1);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == -4);

            REQUIRE(jet[4] == approximately(3 / fp_t{2}));
            REQUIRE(jet[5] == approximately(fp_t{-2}));

            REQUIRE(jet[6] == approximately(2 / fp_t{-4}));
            REQUIRE(jet[7] == approximately(1 / fp_t{-4}));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {y / 2_dbl, x / -4_dbl}, 2, 2);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{1}, fp_t{3}, fp_t{-4}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);

            REQUIRE(jet[2] == 3);

            REQUIRE(jet[4] == approximately(3 / fp_t{2}));

            REQUIRE(jet[6] == approximately(2 / fp_t{-4}));

            REQUIRE(jet[8] == approximately(fp_t{1} / 2 * (jet[6] / 2)));

            REQUIRE(jet[10] == approximately(fp_t{1} / 2 * (jet[4] / -4)));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {y / 2_dbl, x / -4_dbl}, 3, 3);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{1}, fp_t{-5}, fp_t{3}, fp_t{-4}, fp_t{2}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 1);
            REQUIRE(jet[2] == -5);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == -4);
            REQUIRE(jet[5] == 2);

            REQUIRE(jet[6] == approximately(3 / fp_t{2}));
            REQUIRE(jet[7] == approximately(fp_t{-2}));
            REQUIRE(jet[8] == approximately(fp_t{1}));

            REQUIRE(jet[9] == approximately(2 / fp_t{-4}));
            REQUIRE(jet[10] == approximately(1 / fp_t{-4}));
            REQUIRE(jet[11] == approximately(-5 / fp_t{-4}));

            REQUIRE(jet[12] == approximately(fp_t{1} / 2 * (jet[9] / 2)));
            REQUIRE(jet[13] == approximately(fp_t{1} / 2 * (jet[10] / 2)));
            REQUIRE(jet[14] == approximately(fp_t{1} / 2 * (jet[11] / 2)));

            REQUIRE(jet[15] == approximately(fp_t{1} / 2 * (jet[6] / -4)));
            REQUIRE(jet[16] == approximately(fp_t{1} / 2 * (jet[7] / -4)));
            REQUIRE(jet[17] == approximately(fp_t{1} / 2 * (jet[8] / -4)));

            REQUIRE(jet[18] == approximately(fp_t{1} / 6 * jet[15]));
            REQUIRE(jet[19] == approximately(fp_t{1} / 6 * jet[16]));
            REQUIRE(jet[20] == approximately(fp_t{1} / 6 * jet[17]));

            REQUIRE(jet[21] == approximately(fp_t{1} / 6 * (jet[12] * fp_t{2} / fp_t{-4})));
            REQUIRE(jet[22] == approximately(fp_t{1} / 6 * (jet[13] * fp_t{2} / fp_t{-4})));
            REQUIRE(jet[23] == approximately(fp_t{1} / 6 * (jet[14] * fp_t{2} / fp_t{-4})));
        }

        compare_batch_scalar<fp_t>({y / 2_dbl, x / -4_dbl});

        // Number/variable tests.
        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {2_dbl / y, -4_dbl / x}, 1, 1);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(2 / fp_t{3}));
            REQUIRE(jet[3] == approximately(-4 / fp_t{2}));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {2_dbl / y, -4_dbl / x}, 2, 1);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(2 / fp_t{3}));
            REQUIRE(jet[3] == approximately(-4 / fp_t{2}));
            REQUIRE(jet[4] == approximately(-jet[3] / (3 * 3)));
            REQUIRE(jet[5] == approximately(2 * jet[2] / (2 * 2)));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {2_dbl / y, -4_dbl / x}, 1, 2);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{-4}, fp_t{3}, fp_t{5}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -4);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 5);

            REQUIRE(jet[4] == approximately(2 / fp_t{3}));
            REQUIRE(jet[5] == approximately(2 / fp_t{5}));

            REQUIRE(jet[6] == approximately(-4 / fp_t{2}));
            REQUIRE(jet[7] == approximately(-4 / fp_t{-4}));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {2_dbl / y, -4_dbl / x}, 2, 2);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{-4}, fp_t{3}, fp_t{5}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -4);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 5);

            REQUIRE(jet[4] == approximately(2 / fp_t{3}));
            REQUIRE(jet[5] == approximately(2 / fp_t{5}));

            REQUIRE(jet[6] == approximately(-4 / fp_t{2}));
            REQUIRE(jet[7] == approximately(-4 / fp_t{-4}));

            REQUIRE(jet[8] == approximately(-jet[6] / (3 * 3)));
            REQUIRE(jet[9] == approximately(-jet[7] / (5 * 5)));

            REQUIRE(jet[10] == approximately(2 * jet[4] / (2 * 2)));
            REQUIRE(jet[11] == approximately(2 * jet[5] / (4 * 4)));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {2_dbl / y, -4_dbl / x}, 3, 3);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{-4}, fp_t{1}, fp_t{3}, fp_t{5}, fp_t{-2}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -4);
            REQUIRE(jet[2] == 1);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == 5);
            REQUIRE(jet[5] == -2);

            REQUIRE(jet[6] == approximately(2 / fp_t{3}));
            REQUIRE(jet[7] == approximately(2 / fp_t{5}));
            REQUIRE(jet[8] == approximately(2 / fp_t{-2}));

            REQUIRE(jet[9] == approximately(-4 / fp_t{2}));
            REQUIRE(jet[10] == approximately(-4 / fp_t{-4}));
            REQUIRE(jet[11] == approximately(-4 / fp_t{1}));

            REQUIRE(jet[12] == approximately(-jet[9] / (3 * 3)));
            REQUIRE(jet[13] == approximately(-jet[10] / (5 * 5)));
            REQUIRE(jet[14] == approximately(-jet[11] / (2 * 2)));

            REQUIRE(jet[15] == approximately(2 * jet[6] / (2 * 2)));
            REQUIRE(jet[16] == approximately(2 * jet[7] / (4 * 4)));
            REQUIRE(jet[17] == approximately(2 * jet[8] / (1 * 1)));

            REQUIRE(jet[18]
                    == approximately(-1 / fp_t{3} * (2 * jet[15] * 3 * 3 - jet[9] * 2 * 3 * jet[9]) / (3 * 3 * 3 * 3)));
            REQUIRE(
                jet[19]
                == approximately(-1 / fp_t{3} * (2 * jet[16] * 5 * 5 - jet[10] * 2 * 5 * jet[10]) / (5 * 5 * 5 * 5)));
            REQUIRE(
                jet[20]
                == approximately(-1 / fp_t{3} * (2 * jet[17] * 2 * 2 - jet[11] * 2 * -2 * jet[11]) / (2 * 2 * 2 * 2)));

            REQUIRE(jet[21]
                    == approximately(4 / fp_t{6} * (2 * jet[12] * 2 * 2 - jet[6] * 2 * 2 * jet[6]) / (2 * 2 * 2 * 2)));
            REQUIRE(jet[22]
                    == approximately(4 / fp_t{6} * (2 * jet[13] * 4 * 4 + jet[7] * 2 * 4 * jet[7]) / (4 * 4 * 4 * 4)));
            REQUIRE(jet[22]
                    == approximately(4 / fp_t{6} * (2 * jet[14] * 1 * 1 - jet[8] * 2 * 1 * jet[8]) / (1 * 1 * 1 * 1)));
        }

        compare_batch_scalar<fp_t>({2_dbl / y, -4_dbl / x});

        // Variable/variable tests.
        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {x / y, y / x}, 1, 1);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(4);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(2 / fp_t{3}));
            REQUIRE(jet[3] == approximately(3 / fp_t{2}));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {x / y, y / x}, 2, 1);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{3}};
            jet.resize(6);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == 3);
            REQUIRE(jet[2] == approximately(2 / fp_t{3}));
            REQUIRE(jet[3] == approximately(3 / fp_t{2}));
            REQUIRE(jet[4] == approximately(1 / fp_t{2} * (jet[2] * 3 - jet[3] * 2) / (3 * 3)));
            REQUIRE(jet[5] == approximately(1 / fp_t{2} * (jet[3] * 2 - jet[2] * 3) / (2 * 2)));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {x / y, y / x}, 1, 2);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{-5}, fp_t{3}, fp_t{4}};
            jet.resize(8);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -5);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 4);

            REQUIRE(jet[4] == approximately(2 / fp_t{3}));
            REQUIRE(jet[5] == approximately(-5 / fp_t{4}));

            REQUIRE(jet[6] == approximately(3 / fp_t{2}));
            REQUIRE(jet[7] == approximately(4 / fp_t{-5}));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {x / y, y / x}, 2, 2);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{-5}, fp_t{3}, fp_t{4}};
            jet.resize(12);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -5);

            REQUIRE(jet[2] == 3);
            REQUIRE(jet[3] == 4);

            REQUIRE(jet[4] == approximately(2 / fp_t{3}));
            REQUIRE(jet[5] == approximately(-5 / fp_t{4}));

            REQUIRE(jet[6] == approximately(3 / fp_t{2}));
            REQUIRE(jet[7] == approximately(4 / fp_t{-5}));

            REQUIRE(jet[8] == approximately(1 / fp_t{2} * (jet[4] * 3 - jet[6] * 2) / (3 * 3)));
            REQUIRE(jet[9] == approximately(1 / fp_t{2} * (jet[5] * 4 - jet[7] * -5) / (4 * 4)));

            REQUIRE(jet[10] == approximately(1 / fp_t{2} * (jet[6] * 2 - jet[4] * 3) / (2 * 2)));
            REQUIRE(jet[11] == approximately(1 / fp_t{2} * (jet[7] * -5 - jet[5] * 4) / (5 * 5)));
        }

        {
            llvm_state s{"", 0};

            s.add_taylor_jet_batch<fp_t>("jet", {x / y, y / x}, 3, 3);

            s.compile();

            auto jptr = s.fetch_taylor_jet_batch<fp_t>("jet");

            std::vector<fp_t> jet{fp_t{2}, fp_t{-5}, fp_t{1}, fp_t{3}, fp_t{4}, fp_t{-2}};
            jet.resize(24);

            jptr(jet.data());

            REQUIRE(jet[0] == 2);
            REQUIRE(jet[1] == -5);
            REQUIRE(jet[2] == 1);

            REQUIRE(jet[3] == 3);
            REQUIRE(jet[4] == 4);
            REQUIRE(jet[5] == -2);

            REQUIRE(jet[6] == approximately(2 / fp_t{3}));
            REQUIRE(jet[7] == approximately(-5 / fp_t{4}));
            REQUIRE(jet[8] == approximately(1 / fp_t{-2}));

            REQUIRE(jet[9] == approximately(3 / fp_t{2}));
            REQUIRE(jet[10] == approximately(4 / fp_t{-5}));
            REQUIRE(jet[11] == approximately(-2 / fp_t{1}));

            REQUIRE(jet[12] == approximately(1 / fp_t{2} * (jet[6] * 3 - jet[9] * 2) / (3 * 3)));
            REQUIRE(jet[13] == approximately(1 / fp_t{2} * (jet[7] * 4 - jet[10] * -5) / (4 * 4)));
            REQUIRE(jet[14] == approximately(1 / fp_t{2} * (jet[8] * -2 - jet[11] * 1) / (2 * 2)));

            REQUIRE(jet[15] == approximately(1 / fp_t{2} * (jet[9] * 2 - jet[6] * 3) / (2 * 2)));
            REQUIRE(jet[16] == approximately(1 / fp_t{2} * (jet[10] * -5 - jet[7] * 4) / (5 * 5)));
            REQUIRE(jet[17] == approximately(1 / fp_t{2} * (jet[11] * 1 - jet[8] * -2) / (1 * 1)));

            REQUIRE(jet[18]
                    == approximately(1 / fp_t{6}
                                     * ((2 * jet[12] * 3 + jet[6] * jet[9] - 2 * jet[15] * 2 - jet[9] * jet[6]) * 3 * 3
                                        - 2 * 3 * jet[9] * (jet[6] * 3 - jet[9] * 2))
                                     / (3 * 3 * 3 * 3)));
            REQUIRE(jet[19]
                    == approximately(
                        1 / fp_t{6}
                        * ((2 * jet[13] * 4 - 2 * jet[16] * -5) * 4 * 4 - 2 * 4 * jet[10] * (jet[7] * 4 - jet[10] * -5))
                        / (4 * 4 * 4 * 4)));
            REQUIRE(jet[20]
                    == approximately(1 / fp_t{6}
                                     * ((2 * jet[14] * -2 - 2 * jet[17] * 1) * 2 * 2
                                        - 2 * -2 * jet[11] * (jet[8] * -2 - jet[11] * 1))
                                     / (2 * 2 * 2 * 2)));

            REQUIRE(jet[21]
                    == approximately(1 / fp_t{6}
                                     * ((2 * jet[15] * 2 + jet[9] * jet[6] - 2 * jet[12] * 3 - jet[9] * jet[6]) * 2 * 2
                                        - 2 * 2 * jet[6] * (jet[9] * 2 - jet[6] * 3))
                                     / (2 * 2 * 2 * 2)));
            REQUIRE(jet[22]
                    == approximately(
                        1 / fp_t{6}
                        * ((2 * jet[16] * -5 - 2 * jet[13] * 4) * 5 * 5 - 2 * -5 * jet[7] * (jet[10] * -5 - jet[7] * 4))
                        / (5 * 5 * 5 * 5)));
            REQUIRE(jet[23]
                    == approximately(
                        1 / fp_t{6}
                        * ((2 * jet[17] * 1 - 2 * jet[14] * -2) * 1 * 1 - 2 * 1 * jet[8] * (jet[11] * 1 - jet[8] * -2))
                        / (1 * 1 * 1 * 1)));
        }

        compare_batch_scalar<fp_t>({x / y, y / x});
    };

    tuple_for_each(fp_types, tester);
}
