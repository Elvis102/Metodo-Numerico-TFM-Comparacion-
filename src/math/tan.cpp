// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
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
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <boost/numeric/conversion/cast.hpp>

#include <fmt/format.h>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/sleef.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/detail/taylor_common.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/square.hpp>
#include <heyoka/math/tan.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>
#include <heyoka/variable.hpp>

namespace heyoka
{

namespace detail
{

tan_impl::tan_impl(expression e) : func_base("tan", std::vector{std::move(e)}) {}

tan_impl::tan_impl() : tan_impl(0_dbl) {}

llvm::Value *tan_impl::codegen_dbl(llvm_state &s, const std::vector<llvm::Value *> &args) const
{
    assert(args.size() == 1u);
    assert(args[0] != nullptr);

    if (auto vec_t = llvm::dyn_cast<llvm::VectorType>(args[0]->getType())) {
        if (const auto sfn = sleef_function_name(s.context(), "tan", vec_t->getElementType(),
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

    return call_extern_vec(s, args[0], "tan");
}

llvm::Value *tan_impl::codegen_ldbl(llvm_state &s, const std::vector<llvm::Value *> &args) const
{
    assert(args.size() == 1u);
    assert(args[0] != nullptr);

    return call_extern_vec(s, args[0],
#if defined(_MSC_VER)
                           // NOTE: it seems like the MSVC stdlib does not have a tanl function,
                           // because LLVM complains about the symbol "tanl" not being
                           // defined. Hence, use our own wrapper instead.
                           "heyoka_tanl"
#else
                           "tanl"
#endif
    );
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *tan_impl::codegen_f128(llvm_state &s, const std::vector<llvm::Value *> &args) const
{
    assert(args.size() == 1u);
    assert(args[0] != nullptr);

    return call_extern_vec(s, args[0], "heyoka_tan128");
}

#endif

double tan_impl::eval_dbl(const std::unordered_map<std::string, double> &map, const std::vector<double> &pars) const
{
    assert(args().size() == 1u);

    return std::tan(heyoka::eval_dbl(args()[0], map, pars));
}

long double tan_impl::eval_ldbl(const std::unordered_map<std::string, long double> &map, const std::vector<long double> &pars) const
{
    assert(args().size() == 1u);

    return std::tan(heyoka::eval_ldbl(args()[0], map, pars));
}

#if defined(HEYOKA_HAVE_REAL128)
mppp::real128 tan_impl::eval_f128(const std::unordered_map<std::string, mppp::real128> &map, const std::vector<mppp::real128> &pars) const
{
    assert(args().size() == 1u);

    return mppp::tan(heyoka::eval_f128(args()[0], map, pars));
}
#endif

void tan_impl::eval_batch_dbl(std::vector<double> &out, const std::unordered_map<std::string, std::vector<double>> &map,
                              const std::vector<double> &pars) const
{
    assert(args().size() == 1u);

    heyoka::eval_batch_dbl(out, args()[0], map, pars);
    for (auto &el : out) {
        el = std::tan(el);
    }
}

double tan_impl::eval_num_dbl(const std::vector<double> &a) const
{
    if (a.size() != 1u) {
        using namespace fmt::literals;

        throw std::invalid_argument(
            "Inconsistent number of arguments when computing the numerical value of the "
            "tangent over doubles (1 argument was expected, but {} arguments were provided"_format(a.size()));
    }

    return std::tan(a[0]);
}

double tan_impl::deval_num_dbl(const std::vector<double> &a, std::vector<double>::size_type i) const
{
    if (a.size() != 1u || i != 0u) {
        throw std::invalid_argument("Inconsistent number of arguments or derivative requested when computing the "
                                    "numerical derivative of the tangent");
    }

    return std::tan(a[0]);
}

std::vector<std::pair<expression, std::vector<std::uint32_t>>>::size_type
tan_impl::taylor_decompose(std::vector<std::pair<expression, std::vector<std::uint32_t>>> &u_vars_defs) &&
{
    assert(args().size() == 1u);

    // Decompose the argument.
    auto &arg = *get_mutable_args_it().first;
    if (const auto dres = taylor_decompose_in_place(std::move(arg), u_vars_defs)) {
        arg = expression{variable{"u_" + detail::li_to_string(dres)}};
    }

    // Append the tan decomposition.
    u_vars_defs.emplace_back(tan(std::move(arg)), std::vector<std::uint32_t>{});

    // Append the auxiliary function tan(arg) * tan(arg).
    u_vars_defs.emplace_back(square(expression{variable{"u_" + detail::li_to_string(u_vars_defs.size() - 1u)}}),
                             std::vector<std::uint32_t>{});

    // Add the hidden dep.
    (u_vars_defs.end() - 2)->second.push_back(boost::numeric_cast<std::uint32_t>(u_vars_defs.size() - 1u));

    return u_vars_defs.size() - 2u;
}

namespace
{

// Derivative of tan(number).
template <typename T, typename U, std::enable_if_t<is_num_param_v<U>, int> = 0>
llvm::Value *taylor_diff_tan_impl(llvm_state &s, const tan_impl &f, const std::vector<std::uint32_t> &, const U &num,
                                  const std::vector<llvm::Value *> &, llvm::Value *par_ptr, std::uint32_t,
                                  std::uint32_t order, std::uint32_t, std::uint32_t batch_size)
{
    if (order == 0u) {
        return codegen_from_values<T>(s, f, {taylor_codegen_numparam<T>(s, num, par_ptr, batch_size)});
    } else {
        return vector_splat(s.builder(), codegen<T>(s, number{0.}), batch_size);
    }
}

template <typename T>
llvm::Value *taylor_diff_tan_impl(llvm_state &s, const tan_impl &f, const std::vector<std::uint32_t> &deps,
                                  const variable &var, const std::vector<llvm::Value *> &arr, llvm::Value *,
                                  std::uint32_t n_uvars, std::uint32_t order, std::uint32_t, std::uint32_t batch_size)
{
    auto &builder = s.builder();

    // Fetch the index of the variable.
    const auto u_idx = uname_to_index(var.name());

    if (order == 0u) {
        return codegen_from_values<T>(s, f, {taylor_fetch_diff(arr, u_idx, 0, n_uvars)});
    }

    // NOTE: iteration in the [1, order] range.
    std::vector<llvm::Value *> sum;
    for (std::uint32_t j = 1; j <= order; ++j) {
        // NOTE: the only hidden dependency contains the index of the
        // u variable whose definition is tan(var) * tan(var).
        auto bj = taylor_fetch_diff(arr, u_idx, j, n_uvars);
        auto cnj = taylor_fetch_diff(arr, deps[0], order - j, n_uvars);

        auto fac = vector_splat(builder, codegen<T>(s, number(static_cast<T>(j))), batch_size);

        // Add j*cnj*bj to the sum.
        sum.push_back(builder.CreateFMul(fac, builder.CreateFMul(cnj, bj)));
    }

    // Init the return value as the result of the sum.
    auto ret_acc = pairwise_sum(builder, sum);

    // Divide by order.
    ret_acc
        = builder.CreateFDiv(ret_acc, vector_splat(builder, codegen<T>(s, number(static_cast<T>(order))), batch_size));

    // Create and return the result.
    return builder.CreateFAdd(taylor_fetch_diff(arr, u_idx, order, n_uvars), ret_acc);
}

// All the other cases.
template <typename T, typename U, std::enable_if_t<!is_num_param_v<U>, int> = 0>
llvm::Value *taylor_diff_tan_impl(llvm_state &, const tan_impl &, const std::vector<std::uint32_t> &, const U &,
                                  const std::vector<llvm::Value *> &, llvm::Value *, std::uint32_t, std::uint32_t,
                                  std::uint32_t, std::uint32_t)
{
    throw std::invalid_argument(
        "An invalid argument type was encountered while trying to build the Taylor derivative of a tangent");
}

template <typename T>
llvm::Value *taylor_diff_tan(llvm_state &s, const tan_impl &f, const std::vector<std::uint32_t> &deps,
                             const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, std::uint32_t n_uvars,
                             std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    assert(f.args().size() == 1u);

    if (deps.size() != 1u) {
        using namespace fmt::literals;

        throw std::invalid_argument(
            "A hidden dependency vector of size 1 is expected in order to compute the Taylor "
            "derivative of the tangent, but a vector of size {} was passed instead"_format(deps.size()));
    }

    return std::visit(
        [&](const auto &v) {
            return taylor_diff_tan_impl<T>(s, f, deps, v, arr, par_ptr, n_uvars, order, idx, batch_size);
        },
        f.args()[0].value());
}

} // namespace

llvm::Value *tan_impl::taylor_diff_dbl(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                       const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                       std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                       std::uint32_t batch_size) const
{
    return taylor_diff_tan<double>(s, *this, deps, arr, par_ptr, n_uvars, order, idx, batch_size);
}

llvm::Value *tan_impl::taylor_diff_ldbl(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                        const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                        std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                        std::uint32_t batch_size) const
{
    return taylor_diff_tan<long double>(s, *this, deps, arr, par_ptr, n_uvars, order, idx, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *tan_impl::taylor_diff_f128(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                        const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                        std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                        std::uint32_t batch_size) const
{
    return taylor_diff_tan<mppp::real128>(s, *this, deps, arr, par_ptr, n_uvars, order, idx, batch_size);
}

#endif

namespace
{

// Derivative of tan(number).
template <typename T, typename U, std::enable_if_t<is_num_param_v<U>, int> = 0>
llvm::Function *taylor_c_diff_func_tan_impl(llvm_state &s, const tan_impl &fn, const U &num, std::uint32_t,
                                            std::uint32_t batch_size)
{
    using namespace fmt::literals;

    return taylor_c_diff_func_unary_num_det<T>(
        s, fn, num, batch_size,
        "heyoka_taylor_diff_tan_{}_{}"_format(taylor_c_diff_numparam_mangle(num),
                                              taylor_mangle_suffix(to_llvm_vector_type<T>(s.context(), batch_size))),
        "the tangent", 1);
}

// Derivative of tan(variable).
template <typename T>
llvm::Function *taylor_c_diff_func_tan_impl(llvm_state &s, const tan_impl &fn, const variable &, std::uint32_t n_uvars,
                                            std::uint32_t batch_size)
{
    auto &module = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the floating-point type.
    auto val_t = to_llvm_vector_type<T>(context, batch_size);

    // Get the function name.
    const auto fname
        = "heyoka_taylor_diff_tan_var_" + taylor_mangle_suffix(val_t) + "_n_uvars_" + li_to_string(n_uvars);

    // The function arguments:
    // - diff order,
    // - idx of the u variable whose diff is being computed,
    // - diff array,
    // - par ptr,
    // - time ptr,
    // - idx of the var argument,
    // - idx of the uvar whose definition is tan(var) * tan(var).
    std::vector<llvm::Type *> fargs{llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context),
                                    llvm::PointerType::getUnqual(val_t),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::Type::getInt32Ty(context),
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
        auto dep_idx = f->args().begin() + 6;

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

                // Run the loop.
                llvm_loop_u32(s, builder.getInt32(1), builder.CreateAdd(ord, builder.getInt32(1)), [&](llvm::Value *j) {
                    auto bj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, var_idx);
                    auto cnj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), dep_idx);

                    auto fac = vector_splat(builder, builder.CreateUIToFP(j, to_llvm_type<T>(context)), batch_size);

                    builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(acc),
                                                           builder.CreateFMul(fac, builder.CreateFMul(cnj, bj))),
                                        acc);
                });

                // Divide by the order and add to b^[n] to produce the return value.
                auto ord_v = vector_splat(builder, builder.CreateUIToFP(ord, to_llvm_type<T>(context)), batch_size);

                builder.CreateStore(builder.CreateFAdd(taylor_c_load_diff(s, diff_ptr, n_uvars, ord, var_idx),
                                                       builder.CreateFDiv(builder.CreateLoad(acc), ord_v)),
                                    retval);
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
            throw std::invalid_argument(
                "Inconsistent function signature for the Taylor derivative of the tangent in compact mode detected");
        }
    }

    return f;
}

// All the other cases.
template <typename T, typename U, std::enable_if_t<!is_num_param_v<U>, int> = 0>
llvm::Function *taylor_c_diff_func_tan_impl(llvm_state &, const tan_impl &, const U &, std::uint32_t, std::uint32_t)
{
    throw std::invalid_argument("An invalid argument type was encountered while trying to build the Taylor derivative "
                                "of a tangent in compact mode");
}

template <typename T>
llvm::Function *taylor_c_diff_func_tan(llvm_state &s, const tan_impl &fn, std::uint32_t n_uvars,
                                       std::uint32_t batch_size)
{
    assert(fn.args().size() == 1u);

    return std::visit([&](const auto &v) { return taylor_c_diff_func_tan_impl<T>(s, fn, v, n_uvars, batch_size); },
                      fn.args()[0].value());
}

} // namespace

llvm::Function *tan_impl::taylor_c_diff_func_dbl(llvm_state &s, std::uint32_t n_uvars, std::uint32_t batch_size) const
{
    return taylor_c_diff_func_tan<double>(s, *this, n_uvars, batch_size);
}

llvm::Function *tan_impl::taylor_c_diff_func_ldbl(llvm_state &s, std::uint32_t n_uvars, std::uint32_t batch_size) const
{
    return taylor_c_diff_func_tan<long double>(s, *this, n_uvars, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Function *tan_impl::taylor_c_diff_func_f128(llvm_state &s, std::uint32_t n_uvars, std::uint32_t batch_size) const
{
    return taylor_c_diff_func_tan<mppp::real128>(s, *this, n_uvars, batch_size);
}

#endif

expression tan_impl::diff(const std::string &s) const
{
    assert(args().size() == 1u);

    // NOTE: if single-precision floats are implemented,
    // should 1_dbl become 1_flt?
    return (1_dbl + square(tan(args()[0]))) * heyoka::diff(args()[0], s);
}

} // namespace detail

expression tan(expression e)
{
    return expression{func{detail::tan_impl(std::move(e))}};
}

} // namespace heyoka
