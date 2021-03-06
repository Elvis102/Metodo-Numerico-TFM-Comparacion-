// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <cassert>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/detail/taylor_common.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/square.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>
#include <heyoka/variable.hpp>

namespace heyoka
{

namespace detail
{

square_impl::square_impl(expression e) : func_base("square", std::vector{std::move(e)}) {}

square_impl::square_impl() : square_impl(0_dbl) {}

llvm::Value *square_impl::codegen_dbl(llvm_state &s, const std::vector<llvm::Value *> &args) const
{
    assert(args.size() == 1u);
    assert(args[0] != nullptr);

    return s.builder().CreateFMul(args[0], args[0]);
}

llvm::Value *square_impl::codegen_ldbl(llvm_state &s, const std::vector<llvm::Value *> &args) const
{
    // NOTE: codegen is identical as in dbl.
    return codegen_dbl(s, args);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *square_impl::codegen_f128(llvm_state &s, const std::vector<llvm::Value *> &args) const
{
    // NOTE: codegen is identical as in dbl.
    return codegen_dbl(s, args);
}

#endif

double square_impl::eval_dbl(const std::unordered_map<std::string, double> &map, const std::vector<double> &pars) const
{
    assert(args().size() == 1u);

    return heyoka::eval_dbl(args()[0], map, pars)*heyoka::eval_dbl(args()[0], map, pars);
}

long double square_impl::eval_ldbl(const std::unordered_map<std::string, long double> &map, const std::vector<long double> &pars) const
{
    assert(args().size() == 1u);

    return heyoka::eval_ldbl(args()[0], map, pars)*heyoka::eval_ldbl(args()[0], map, pars);
}

#if defined(HEYOKA_HAVE_REAL128)
mppp::real128 square_impl::eval_f128(const std::unordered_map<std::string, mppp::real128> &map, const std::vector<mppp::real128> &pars) const
{
    assert(args().size() == 1u);

    return heyoka::eval_f128(args()[0], map, pars)*heyoka::eval_f128(args()[0], map, pars);
}
#endif

namespace
{

// Derivative of square(number).
template <typename T, typename U, std::enable_if_t<is_num_param_v<U>, int> = 0>
llvm::Value *taylor_diff_square_impl(llvm_state &s, const square_impl &f, const U &num,
                                     const std::vector<llvm::Value *> &, llvm::Value *par_ptr, std::uint32_t,
                                     std::uint32_t order, std::uint32_t, std::uint32_t batch_size)
{
    if (order == 0u) {
        return codegen_from_values<T>(s, f, {taylor_codegen_numparam<T>(s, num, par_ptr, batch_size)});
    } else {
        return vector_splat(s.builder(), codegen<T>(s, number{0.}), batch_size);
    }
}

// Derivative of square(variable).
template <typename T>
llvm::Value *taylor_diff_square_impl(llvm_state &s, const square_impl &f, const variable &var,
                                     const std::vector<llvm::Value *> &arr, llvm::Value *, std::uint32_t n_uvars,
                                     std::uint32_t order, std::uint32_t, std::uint32_t)
{
    auto &builder = s.builder();

    // Fetch the index of the variable.
    const auto u_idx = uname_to_index(var.name());

    if (order == 0u) {
        return codegen_from_values<T>(s, f, {taylor_fetch_diff(arr, u_idx, 0, n_uvars)});
    }

    // Compute the sum.
    std::vector<llvm::Value *> sum;
    if (order % 2u == 1u) {
        // Odd order.
        for (std::uint32_t j = 0; j <= (order - 1u) / 2u; ++j) {
            auto v0 = taylor_fetch_diff(arr, u_idx, order - j, n_uvars);
            auto v1 = taylor_fetch_diff(arr, u_idx, j, n_uvars);

            sum.push_back(builder.CreateFMul(v0, v1));
        }

        auto ret = pairwise_sum(builder, sum);
        return builder.CreateFAdd(ret, ret);
    } else {
        // Even order.
        auto ak2 = taylor_fetch_diff(arr, u_idx, order / 2u, n_uvars);
        auto sq_ak2 = builder.CreateFMul(ak2, ak2);

        for (std::uint32_t j = 0; j <= (order - 2u) / 2u; ++j) {
            auto v0 = taylor_fetch_diff(arr, u_idx, order - j, n_uvars);
            auto v1 = taylor_fetch_diff(arr, u_idx, j, n_uvars);

            sum.push_back(builder.CreateFMul(v0, v1));
        }

        auto ret = pairwise_sum(builder, sum);
        return builder.CreateFAdd(builder.CreateFAdd(ret, ret), sq_ak2);
    }
}

// All the other cases.
template <typename T, typename U, std::enable_if_t<!is_num_param_v<U>, int> = 0>
llvm::Value *taylor_diff_square_impl(llvm_state &, const square_impl &, const U &, const std::vector<llvm::Value *> &,
                                     llvm::Value *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)
{
    throw std::invalid_argument(
        "An invalid argument type was encountered while trying to build the Taylor derivative of a square");
}

template <typename T>
llvm::Value *taylor_diff_square(llvm_state &s, const square_impl &f, const std::vector<std::uint32_t> &deps,
                                const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, std::uint32_t n_uvars,
                                std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    assert(f.args().size() == 1u);

    if (!deps.empty()) {
        using namespace fmt::literals;

        throw std::invalid_argument("An empty hidden dependency vector is expected in order to compute the Taylor "
                                    "derivative of the square, but a vector of size {} was passed "
                                    "instead"_format(deps.size()));
    }

    return std::visit(
        [&](const auto &v) {
            return taylor_diff_square_impl<T>(s, f, v, arr, par_ptr, n_uvars, order, idx, batch_size);
        },
        f.args()[0].value());
}

} // namespace

llvm::Value *square_impl::taylor_diff_dbl(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                          const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                          std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                          std::uint32_t batch_size) const
{
    return taylor_diff_square<double>(s, *this, deps, arr, par_ptr, n_uvars, order, idx, batch_size);
}

llvm::Value *square_impl::taylor_diff_ldbl(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                           const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                           std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                           std::uint32_t batch_size) const
{
    return taylor_diff_square<long double>(s, *this, deps, arr, par_ptr, n_uvars, order, idx, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *square_impl::taylor_diff_f128(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                           const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                           std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                           std::uint32_t batch_size) const
{
    return taylor_diff_square<mppp::real128>(s, *this, deps, arr, par_ptr, n_uvars, order, idx, batch_size);
}

#endif

namespace
{

// Derivative of square(number).
template <typename T, typename U, std::enable_if_t<is_num_param_v<U>, int> = 0>
llvm::Function *taylor_c_diff_func_square_impl(llvm_state &s, const square_impl &fn, const U &num, std::uint32_t,
                                               std::uint32_t batch_size)
{
    using namespace fmt::literals;

    return taylor_c_diff_func_unary_num_det<T>(
        s, fn, num, batch_size,
        "heyoka_taylor_diff_square_{}_{}"_format(taylor_c_diff_numparam_mangle(num),
                                                 taylor_mangle_suffix(to_llvm_vector_type<T>(s.context(), batch_size))),
        "the square");
}

// Derivative of square(variable).
template <typename T>
llvm::Function *taylor_c_diff_func_square_impl(llvm_state &s, const square_impl &fn, const variable &,
                                               std::uint32_t n_uvars, std::uint32_t batch_size)
{
    auto &module = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the floating-point type.
    auto val_t = to_llvm_vector_type<T>(context, batch_size);

    // Get the function name.
    using namespace fmt::literals;
    const auto fname = "heyoka_taylor_diff_square_var_{}_n_uvars_{}"_format(taylor_mangle_suffix(val_t), n_uvars);

    // The function arguments:
    // - diff order,
    // - idx of the u variable whose diff is being computed,
    // - diff array,
    // - par ptr,
    // - time ptr,
    // - idx of the var argument.
    std::vector<llvm::Type *> fargs{llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context),
                                    llvm::PointerType::getUnqual(val_t),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::Type::getInt32Ty(context)};

    // Try to see if we already created the function.
    auto f = module.getFunction(fname);

    if (f == nullptr) {
        // The function was not created before, do it now.

        // Fetch the current insertion block.
        auto orig_bb = builder.GetInsertBlock();

        // The return type is val_t.
        auto *ft = llvm::FunctionType::get(val_t, fargs, false);
        // Create the function
        f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fname, &module);
        assert(f != nullptr);

        // Fetch the necessary function arguments.
        auto ord = f->args().begin();
        auto diff_ptr = f->args().begin() + 2;
        auto var_idx = f->args().begin() + 5;

        // Create a new basic block to start insertion into.
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", f));

        // Create the return value.
        auto retval = builder.CreateAlloca(val_t);

        // Create the accumulator.
        auto acc = builder.CreateAlloca(val_t);

        llvm_if_then_else(
            s, builder.CreateICmpEQ(ord, builder.getInt32(0)),
            [&]() {
                // For order 0, invoke the function on the order 0 of var_idx.
                builder.CreateStore(
                    codegen_from_values<T>(s, fn,
                                           {taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), var_idx)}),
                    retval);
            },
            [&]() {
                // Init the accumulator.
                builder.CreateStore(vector_splat(builder, codegen<T>(s, number{0.}), batch_size), acc);

                // Distinguish the odd/even cases for the order.
                llvm_if_then_else(
                    s, builder.CreateICmpEQ(builder.CreateURem(ord, builder.getInt32(2)), builder.getInt32(1)),
                    [&]() {
                        // Odd order.
                        auto loop_end = builder.CreateAdd(
                            builder.CreateUDiv(builder.CreateSub(ord, builder.getInt32(1)), builder.getInt32(2)),
                            builder.getInt32(1));
                        llvm_loop_u32(s, builder.getInt32(0), loop_end, [&](llvm::Value *j) {
                            auto a_nj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), var_idx);
                            auto aj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, var_idx);

                            builder.CreateStore(
                                builder.CreateFAdd(builder.CreateLoad(acc), builder.CreateFMul(a_nj, aj)), acc);
                        });

                        // Return 2 * acc.
                        auto acc_load = builder.CreateLoad(acc);
                        builder.CreateStore(builder.CreateFAdd(acc_load, acc_load), retval);
                    },
                    [&]() {
                        // Even order.

                        // Pre-compute the final term.
                        auto ak2 = taylor_c_load_diff(s, diff_ptr, n_uvars,
                                                      builder.CreateUDiv(ord, builder.getInt32(2)), var_idx);
                        auto sq_ak2 = builder.CreateFMul(ak2, ak2);

                        auto loop_end = builder.CreateAdd(
                            builder.CreateUDiv(builder.CreateSub(ord, builder.getInt32(2)), builder.getInt32(2)),
                            builder.getInt32(1));
                        llvm_loop_u32(s, builder.getInt32(0), loop_end, [&](llvm::Value *j) {
                            auto a_nj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), var_idx);
                            auto aj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, var_idx);

                            builder.CreateStore(
                                builder.CreateFAdd(builder.CreateLoad(acc), builder.CreateFMul(a_nj, aj)), acc);
                        });

                        // Return 2 * acc + ak2 * ak2.
                        auto acc_load = builder.CreateLoad(acc);
                        builder.CreateStore(builder.CreateFAdd(builder.CreateFAdd(acc_load, acc_load), sq_ak2), retval);
                    });
            });

        // Return the result.
        builder.CreateRet(builder.CreateLoad(retval));

        // Verify.
        s.verify_function(f);

        // Restore the original insertion block.
        builder.SetInsertPoint(orig_bb);
    } else {
        // The function was created before. Check if the signatures match.
        // NOTE: there could be a mismatch if the derivative function was created
        // and then optimised - optimisation might remove arguments which are compile-time
        // constants.
        if (!compare_function_signature(f, val_t, fargs)) {
            throw std::invalid_argument("Inconsistent function signature for the Taylor derivative of the square "
                                        "in compact mode detected");
        }
    }

    return f;
}

// All the other cases.
template <typename T, typename U, std::enable_if_t<!is_num_param_v<U>, int> = 0>
llvm::Function *taylor_c_diff_func_square_impl(llvm_state &, const square_impl &, const U &, std::uint32_t,
                                               std::uint32_t)
{
    throw std::invalid_argument("An invalid argument type was encountered while trying to build the Taylor derivative "
                                "of a square in compact mode");
}

template <typename T>
llvm::Function *taylor_c_diff_func_square(llvm_state &s, const square_impl &fn, std::uint32_t n_uvars,
                                          std::uint32_t batch_size)
{
    assert(fn.args().size() == 1u);

    return std::visit([&](const auto &v) { return taylor_c_diff_func_square_impl<T>(s, fn, v, n_uvars, batch_size); },
                      fn.args()[0].value());
}

} // namespace

llvm::Function *square_impl::taylor_c_diff_func_dbl(llvm_state &s, std::uint32_t n_uvars,
                                                    std::uint32_t batch_size) const
{
    return taylor_c_diff_func_square<double>(s, *this, n_uvars, batch_size);
}

llvm::Function *square_impl::taylor_c_diff_func_ldbl(llvm_state &s, std::uint32_t n_uvars,
                                                     std::uint32_t batch_size) const
{
    return taylor_c_diff_func_square<long double>(s, *this, n_uvars, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Function *square_impl::taylor_c_diff_func_f128(llvm_state &s, std::uint32_t n_uvars,
                                                     std::uint32_t batch_size) const
{
    return taylor_c_diff_func_square<mppp::real128>(s, *this, n_uvars, batch_size);
}

#endif

expression square_impl::diff(const std::string &s) const
{
    assert(args().size() == 1u);

    return 2_dbl * args()[0] * heyoka::diff(args()[0], s);
}

} // namespace detail

expression square(expression e)
{
    return expression{func{detail::square_impl(std::move(e))}};
}

} // namespace heyoka
