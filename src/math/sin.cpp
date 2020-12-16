// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <boost/numeric/conversion/cast.hpp>

#include <fmt/format.h>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/sleef.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/sin.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>
#include <heyoka/variable.hpp>

namespace heyoka
{

namespace detail
{

sin_impl::sin_impl(expression e) : func_base("sin", std::vector{std::move(e)}) {}

sin_impl::sin_impl() : sin_impl(0_dbl) {}

llvm::Value *sin_impl::codegen_dbl(llvm_state &s, const std::vector<llvm::Value *> &args) const
{
    assert(args.size() == 1u);
    assert(args[0] != nullptr);

    if (auto vec_t = llvm::dyn_cast<llvm::VectorType>(args[0]->getType())) {
        if (const auto sfn = sleef_function_name(s.context(), "sin", vec_t->getElementType(),
                                                 boost::numeric_cast<std::uint32_t>(vec_t->getNumElements()));
            !sfn.empty()) {
            return llvm_invoke_external(
                s, sfn, vec_t, args,
                // NOTE: in theory we may add ReadNone here as well,
                // but for some reason, at least up to LLVM 10,
                // this causes strange codegen issues. Revisit
                // in the future.
                {llvm::Attribute::NoUnwind, llvm::Attribute::Speculatable, llvm::Attribute::WillReturn});
        }
    }

    return llvm_invoke_intrinsic(s, "llvm.sin", {args[0]->getType()}, args);
}

llvm::Value *sin_impl::codegen_ldbl(llvm_state &s, const std::vector<llvm::Value *> &args) const
{
    assert(args.size() == 1u);
    assert(args[0] != nullptr);

    return llvm_invoke_intrinsic(s, "llvm.sin", {args[0]->getType()}, args);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *sin_impl::codegen_f128(llvm_state &s, const std::vector<llvm::Value *> &args) const
{
    assert(args.size() == 1u);
    assert(args[0] != nullptr);

    auto &builder = s.builder();

    // Decompose the argument into scalars.
    auto scalars = vector_to_scalars(builder, args[0]);

    // Invoke the function on each scalar.
    std::vector<llvm::Value *> retvals;
    for (auto scal : scalars) {
        retvals.push_back(llvm_invoke_external(
            s, "heyoka_sin128", scal->getType(), {scal},
            // NOTE: in theory we may add ReadNone here as well,
            // but for some reason, at least up to LLVM 10,
            // this causes strange codegen issues. Revisit
            // in the future.
            {llvm::Attribute::NoUnwind, llvm::Attribute::Speculatable, llvm::Attribute::WillReturn}));
    }

    // Build a vector with the results.
    return scalars_to_vector(builder, retvals);
}

#endif

double sin_impl::eval_dbl(const std::unordered_map<std::string, double> &map) const
{
    assert(args().size() == 1u);

    return std::sin(heyoka::eval_dbl(args()[0], map));
}

void sin_impl::eval_batch_dbl(std::vector<double> &out,
                              const std::unordered_map<std::string, std::vector<double>> &map) const
{
    assert(args().size() == 1u);

    heyoka::eval_batch_dbl(out, args()[0], map);
    for (auto &el : out) {
        el = std::sin(el);
    }
}

double sin_impl::eval_num_dbl(const std::vector<double> &a) const
{
    if (a.size() != 1u) {
        using namespace fmt::literals;

        throw std::invalid_argument(
            "Inconsistent number of arguments when computing the numerical value of the "
            "sine over doubles (1 argument was expected, but {} arguments were provided"_format(a.size()));
    }

    return std::sin(a[0]);
}

double sin_impl::deval_num_dbl(const std::vector<double> &a, std::vector<double>::size_type i) const
{
    if (a.size() != 1u || i != 0u) {
        throw std::invalid_argument("Inconsistent number of arguments or derivative requested when computing "
                                    "the numerical derivative of the sine");
    }

    return std::cos(a[0]);
}

namespace
{

// Derivative of sin(number).
template <typename T>
llvm::Value *taylor_diff_sin_impl(llvm_state &s, const number &, const std::vector<llvm::Value *> &, std::uint32_t,
                                  std::uint32_t, std::uint32_t, std::uint32_t batch_size)
{
    return vector_splat(s.builder(), codegen<T>(s, number{0.}), batch_size);
}

// Derivative of sin(variable).
template <typename T>
llvm::Value *taylor_diff_sin_impl(llvm_state &s, const variable &var, const std::vector<llvm::Value *> &arr,
                                  std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                  std::uint32_t batch_size)
{
    // NOTE: pairwise summation requires order 1 at least.
    // NOTE: also not much use in allowing zero-order
    // derivatives, which in general might complicate
    // the implementation.
    if (order == 0u) {
        throw std::invalid_argument(
            "Cannot compute the Taylor derivative of order 0 of sin() (the order must be at least one)");
    }

    // Fetch the index of the variable.
    const auto u_idx = uname_to_index(var.name());

    // NOTE: iteration in the [1, order] range
    // (i.e., order included).
    std::vector<llvm::Value *> sum;
    auto &builder = s.builder();
    for (std::uint32_t j = 1; j <= order; ++j) {
        // NOTE: the +1 is because we are accessing the cosine
        // of the u var, which is conventionally placed
        // right after the sine in the decomposition.
        auto v0 = taylor_fetch_diff(arr, idx + 1u, order - j, n_uvars);
        auto v1 = taylor_fetch_diff(arr, u_idx, j, n_uvars);

        auto fac = vector_splat(builder, codegen<T>(s, number(static_cast<T>(j))), batch_size);

        // Add j*v0*v1 to the sum.
        sum.push_back(builder.CreateFMul(fac, builder.CreateFMul(v0, v1)));
    }

    // Init the return value as the result of the sum.
    auto ret_acc = pairwise_sum(builder, sum);

    // Compute and return the result: ret_acc / order
    auto div = vector_splat(builder, codegen<T>(s, number(static_cast<T>(order))), batch_size);

    return builder.CreateFDiv(ret_acc, div);
}

// All the other cases.
template <typename T, typename U>
llvm::Value *taylor_diff_sin_impl(llvm_state &, const U &, const std::vector<llvm::Value *> &, std::uint32_t,
                                  std::uint32_t, std::uint32_t, std::uint32_t)
{
    throw std::invalid_argument(
        "An invalid argument type was encountered while trying to build the Taylor derivative of a sine");
}

template <typename T>
llvm::Value *taylor_diff_sin(llvm_state &s, const sin_impl &f, const std::vector<llvm::Value *> &arr,
                             std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    assert(f.args().size() == 1u);

    return std::visit(
        [&](const auto &v) { return taylor_diff_sin_impl<T>(s, v, arr, n_uvars, order, idx, batch_size); },
        f.args()[0].value());
}

} // namespace

llvm::Value *sin_impl::taylor_diff_dbl(llvm_state &s, const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars,
                                       std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size) const
{
    return taylor_diff_sin<double>(s, *this, arr, n_uvars, order, idx, batch_size);
}

llvm::Value *sin_impl::taylor_diff_ldbl(llvm_state &s, const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars,
                                        std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size) const
{
    return taylor_diff_sin<long double>(s, *this, arr, n_uvars, order, idx, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *sin_impl::taylor_diff_f128(llvm_state &s, const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars,
                                        std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size) const
{
    return taylor_diff_sin<mppp::real128>(s, *this, arr, n_uvars, order, idx, batch_size);
}

#endif

} // namespace detail

} // namespace heyoka
