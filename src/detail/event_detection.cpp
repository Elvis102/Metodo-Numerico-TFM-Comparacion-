// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/math/policies/policy.hpp>
#include <boost/math/special_functions/binomial.hpp>
#include <boost/math/tools/toms748_solve.hpp>
#include <boost/numeric/conversion/cast.hpp>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <fmt/ostream.h>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <heyoka/detail/event_detection.hpp>
#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/logging_impl.hpp>
#include <heyoka/detail/type_traits.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/number.hpp>
#include <heyoka/taylor.hpp>

namespace heyoka::detail
{

namespace
{

// Helper to fetch the per-thread poly cache.
template <typename T>
auto &get_poly_cache()
{
    thread_local std::vector<std::vector<std::vector<T>>> ret;

    return ret;
}

// Extract a poly of order n from the cache (or create a new one).
template <typename T>
auto get_poly_from_cache(std::uint32_t n)
{
    // Get/create the thread-local cache.
    auto &cache = get_poly_cache<T>();

    // Look if we have inited the cache for order n.
    if (n >= cache.size()) {
        // The cache was never used for polynomials of order
        // n, add the necessary entries.

        // NOTE: no overflow check needed here, cause the order
        // is always overflow checked in the integrator machinery.
        cache.resize(boost::numeric_cast<decltype(cache.size())>(n + 1u));
    }

    auto &pcache = cache[n];

    if (pcache.empty()) {
        // No polynomials are available, create a new one.
        return std::vector<T>(boost::numeric_cast<typename std::vector<T>::size_type>(n + 1u));
    } else {
        // Extract an existing polynomial from the cache.
        auto retval = std::move(pcache.back());
        pcache.pop_back();

        return retval;
    }
}

// Insert a poly into the cache.
template <typename T>
void put_poly_in_cache(std::vector<T> &&v)
{
    // Get/create the thread-local cache.
    auto &cache = get_poly_cache<T>();

    // Fetch the order of the polynomial.
    // NOTE: the order is the size - 1.
    assert(!v.empty());
    const auto n = v.size() - 1u;

    // Look if we have inited the cache for order n.
    if (n >= cache.size()) {
        // NOTE: this is currently never reached
        // because we always look in the cache
        // before the first invocation of this function.
        // LCOV_EXCL_START
        // The cache was never used for polynomials of order
        // n, add the necessary entries.
        if (n == std::numeric_limits<decltype(cache.size())>::max()) {
            throw std::overflow_error("An overflow was detected in the polynomial cache");
        }
        cache.resize(n + 1u);
        // LCOV_EXCL_STOP
    }

    // Move v in.
    cache[n].push_back(std::move(v));
}

// Given an input polynomial a(x), substitute
// x with x_1 * h and write to ret the resulting
// polynomial in the new variable x_1. Requires
// random-access iterators.
// NOTE: aliasing allowed.
template <typename OutputIt, typename InputIt, typename T>
void poly_rescale(OutputIt ret, InputIt a, const T &scal, std::uint32_t n)
{
    T cur_f(1);

    for (std::uint32_t i = 0; i <= n; ++i) {
        ret[i] = cur_f * a[i];
        cur_f *= scal;
    }
}

// Transform the polynomial a(x) into 2**n * a(x / 2).
// Requires random-access iterators.
// NOTE: aliasing allowed.
template <typename OutputIt, typename InputIt>
void poly_rescale_p2(OutputIt ret, InputIt a, std::uint32_t n)
{
    using value_type = typename std::iterator_traits<InputIt>::value_type;

    value_type cur_f(1);

    for (std::uint32_t i = 0; i <= n; ++i) {
        ret[n - i] = cur_f * a[n - i];
        cur_f *= 2;
    }
}

// Generic branchless sign function.
template <typename T>
int sgn(T val)
{
    return (T(0) < val) - (val < T(0));
}

// Count the number of sign changes in the coefficients of polynomial a.
// Zero coefficients are skipped. Requires random-access iterator.
// NOTE: in case of NaN values in a, the return value of this
// function will be meaningless, and we rely on checks elsewhere
// (e.g., in the root finding function) to bail out.
template <typename InputIt>
std::uint32_t count_sign_changes(InputIt a, std::uint32_t n)
{
    assert(n > 0u);

    using std::isnan;

    std::uint32_t retval = 0;

    // Start from index 0 and move forward
    // until we find a nonzero coefficient.
    std::uint32_t last_nz_idx = 0;
    while (a[last_nz_idx] == 0) {
        if (last_nz_idx == n - 1u) {
            // The second-to-last coefficient is
            // zero, no sign changes are possible
            // regardless of the sign of the
            // last coefficient.
            return 0;
        }
        ++last_nz_idx;
    }

    // Start iterating 1 past the first nonzero coefficient.
    for (auto idx = last_nz_idx + 1u; idx <= n; ++idx) {
        // Determine if a sign change occurred wrt
        // the last nonzero coefficient found.
        const auto cur_sign = sgn(a[idx]);
        // NOTE: don't run the assertion check if
        // we are dealing with nans.
        assert(isnan(a[last_nz_idx]) || sgn(a[last_nz_idx]));
        retval += (cur_sign + sgn(a[last_nz_idx])) == 0;

        // Update last_nz_idx if necessary.
        last_nz_idx = cur_sign ? idx : last_nz_idx;
    }

#if !defined(NDEBUG)
    // In debug mode, run a sanity check with a simpler algorithm.
    thread_local std::vector<typename std::iterator_traits<InputIt>::value_type> nz_cfs;
    nz_cfs.clear();

    // NOTE: check if we have nans in the polynomials,
    // we don't want to run the check in that case.
    bool has_nan = false;

    for (std::uint32_t i = 0; i <= n; ++i) {
        if (isnan(a[i])) {
            has_nan = true;
        }

        if (a[i] != 0) {
            nz_cfs.push_back(a[i]);
        }
    }

    std::uint32_t r_check = 0;
    for (std::uint32_t i = 1; i < nz_cfs.size(); ++i) {
        if ((nz_cfs[i] > 0) != (nz_cfs[i - 1u] > 0)) {
            ++r_check;
        }
    }

    assert(has_nan || r_check == retval);
#endif

    return retval;
}

// Evaluate the first derivative of a polynomial.
// Requires random-access iterator.
template <typename InputIt, typename T>
auto poly_eval_1(InputIt a, T x, std::uint32_t n)
{
    assert(n >= 2u);

    // Init the return value.
    auto ret1 = a[n] * n;

    for (std::uint32_t i = 1; i < n; ++i) {
        ret1 = a[n - i] * (n - i) + ret1 * x;
    }

    return ret1;
}

// Evaluate polynomial.
// Requires random-access iterator.
template <typename InputIt, typename T>
auto poly_eval(InputIt a, T x, std::uint32_t n)
{
    auto ret = a[n];

    for (std::uint32_t i = 1; i <= n; ++i) {
        ret = a[n - i] + ret * x;
    }

    return ret;
}

// A RAII helper to extract polys from the cache and
// return them to the cache upon destruction.
template <typename T>
struct pwrap {
    explicit pwrap(std::uint32_t n) : v(get_poly_from_cache<T>(n)) {}

    // NOTE: upon move, the v of other is guaranteed
    // to become empty().
    pwrap(pwrap &&) noexcept = default;

    // Delete the rest.
    pwrap(const pwrap &) = delete;
    pwrap &operator=(const pwrap &) = delete;
    pwrap &operator=(pwrap &&) = delete;

    ~pwrap()
    {
        // NOTE: put back into cache only
        // if this was not moved-from.
        if (!v.empty()) {
            put_poly_in_cache(std::move(v));
        }
    }

    std::vector<T> v;
};

// Find the only existing root for the polynomial poly of the given order
// existing in [lb, ub].
template <typename T>
std::tuple<T, int> bracketed_root_find(const pwrap<T> &poly, std::uint32_t order, T lb, T ub)
{
    // NOTE: perhaps this should depend on T?
    constexpr boost::uintmax_t iter_limit = 100;
    boost::uintmax_t max_iter = iter_limit;

    // Ensure that root finding does not throw on error,
    // rather it will write something to errno instead.
    // https://www.boost.org/doc/libs/1_75_0/libs/math/doc/html/math_toolkit/pol_tutorial/namespace_policies.html
    using boost::math::policies::domain_error;
    using boost::math::policies::errno_on_error;
    using boost::math::policies::evaluation_error;
    using boost::math::policies::overflow_error;
    using boost::math::policies::pole_error;
    using boost::math::policies::policy;

    using pol = policy<domain_error<errno_on_error>, pole_error<errno_on_error>, overflow_error<errno_on_error>,
                       evaluation_error<errno_on_error>>;

    // Clear out errno before running the root finding.
    errno = 0;

    // Run the root finder.
    const auto p = boost::math::tools::toms748_solve([d = poly.v.data(), order](T x) { return poly_eval(d, x, order); },
                                                     lb, ub, boost::math::tools::eps_tolerance<T>(), max_iter, pol{});
    const auto ret = (p.first + p.second) / 2;

    SPDLOG_LOGGER_DEBUG(get_logger(), "root finding iterations: {}", max_iter);

    if (errno > 0) {
        // Some error condition arose during root finding,
        // return zero and errno.
        return std::tuple{T(0), errno};
    }

    if (max_iter < iter_limit) {
        // Root finding terminated within the
        // iteration limit, return ret and success.
        return std::tuple{ret, 0};
    } else {
        // LCOV_EXCL_START
        // Root finding needed too many iterations,
        // return the (possibly wrong) result
        // and flag -1.
        return std::tuple{ret, -1};
        // LCOV_EXCL_STOP
    }
}

// Helper to fetch the per-thread working list used in
// poly root finding.
template <typename T>
auto &get_wlist()
{
    thread_local std::vector<std::tuple<T, T, pwrap<T>>> w_list;

    return w_list;
}

// Helper to fetch the per-thread list of isolating intervals.
template <typename T>
auto &get_isol()
{
    thread_local std::vector<std::tuple<T, T>> isol;

    return isol;
}

// Helper to detect events of terminal type.
template <typename>
struct is_terminal_event : std::false_type {
};

template <typename T>
struct is_terminal_event<t_event<T>> : std::true_type {
};

template <typename T>
constexpr bool is_terminal_event_v = is_terminal_event<T>::value;

// Helper to compute binomial coefficients
// using Boost.Math.
template <typename T>
auto boost_math_bc(std::uint32_t n_, std::uint32_t k_)
{
    const auto n = boost::numeric_cast<unsigned>(n_);
    const auto k = boost::numeric_cast<unsigned>(k_);

    return boost::math::binomial_coefficient<T>(n, k);
}

// Helper to add a polynomial translation function
// to the state 's'.
template <typename T>
void add_poly_translator_1(llvm_state &s, std::uint32_t order, std::uint32_t batch_size)
{
    assert(order > 0u);

    auto &builder = s.builder();
    auto &context = s.context();

    // The function arguments:
    // - the output pointer (write-only),
    // - the pointer to the poly coefficients (read-only).
    // No overlap is allowed.
    std::vector<llvm::Type *> fargs(2, llvm::PointerType::getUnqual(to_llvm_type<T>(context)));
    // The function does not return anything.
    auto *ft = llvm::FunctionType::get(builder.getVoidTy(), fargs, false);
    assert(ft != nullptr);
    // Now create the function.
    auto *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "poly_translate_1", &s.module());
    // LCOV_EXCL_START
    if (f == nullptr) {
        throw std::invalid_argument("Unable to create a function for polynomial translation");
    }
    // LCOV_EXCL_STOP

    // Set the names/attributes of the function arguments.
    auto out_ptr = f->args().begin();
    out_ptr->setName("out_ptr");
    out_ptr->addAttr(llvm::Attribute::NoCapture);
    out_ptr->addAttr(llvm::Attribute::NoAlias);
    out_ptr->addAttr(llvm::Attribute::WriteOnly);

    auto cf_ptr = f->args().begin() + 1;
    cf_ptr->setName("cf_ptr");
    cf_ptr->addAttr(llvm::Attribute::NoCapture);
    cf_ptr->addAttr(llvm::Attribute::NoAlias);
    cf_ptr->addAttr(llvm::Attribute::ReadOnly);

    // Create a new basic block to start insertion into.
    auto *bb = llvm::BasicBlock::Create(context, "entry", f);
    assert(bb != nullptr);
    builder.SetInsertPoint(bb);

    // Init the return values as zeroes.
    std::vector<llvm::Value *> ret_cfs;
    for (std::uint32_t i = 0; i <= order; ++i) {
        ret_cfs.push_back(vector_splat(builder, codegen<T>(s, number{0.}), batch_size));
    }

    // Do the translation.
    for (std::uint32_t i = 0; i <= order; ++i) {
        auto ai = load_vector_from_memory(
            builder, builder.CreateInBoundsGEP(cf_ptr, {builder.getInt32(i * batch_size)}), batch_size);

        for (std::uint32_t k = 0; k <= i; ++k) {
            auto tmp = builder.CreateFMul(
                ai, vector_splat(builder, codegen<T>(s, number{boost_math_bc<T>(i, k)}), batch_size));

            ret_cfs[k] = builder.CreateFAdd(ret_cfs[k], tmp);
        }
    }

    // Write out the return value.
    for (std::uint32_t i = 0; i <= order; ++i) {
        auto ret_ptr = builder.CreateInBoundsGEP(out_ptr, {builder.getInt32(i * batch_size)});
        store_vector_to_memory(builder, ret_ptr, ret_cfs[i]);
    }

    // Create the return value.
    builder.CreateRetVoid();

    // Verify the function.
    s.verify_function(f);

    // Run the optimisation pass.
    s.optimise();
}

// Fetch a polynomial translation function
// from the thread-local cache.
template <typename T>
auto get_poly_translator_1(std::uint32_t order)
{
    using func_t = void (*)(T *, const T *);

    thread_local std::unordered_map<std::uint32_t, std::pair<llvm_state, func_t>> tf_map;

    auto it = tf_map.find(order);

    if (it == tf_map.end()) {
        // Cache miss, we need to create
        // a new LLVM state and function.
        llvm_state s;

        // Add the polynomial translation function.
        add_poly_translator_1<T>(s, order, 1);

        s.compile();

        // Fetch the function.
        auto f = reinterpret_cast<func_t>(s.jit_lookup("poly_translate_1"));

        // Insert state and function into the cache.
        [[maybe_unused]] const auto ret = tf_map.try_emplace(order, std::pair{std::move(s), f});
        assert(ret.second);

        return f;
    } else {
        // Cache hit, return the function.
        return it->second.second;
    }
}

// Implementation of event detection.
template <typename T>
void taylor_detect_events_impl(std::vector<std::tuple<std::uint32_t, T, bool>> &d_tes,
                               std::vector<std::tuple<std::uint32_t, T>> &d_ntes, const std::vector<t_event<T>> &tes,
                               const std::vector<nt_event<T>> &ntes,
                               const std::vector<std::optional<std::pair<T, T>>> &cooldowns, T h,
                               const std::vector<T> &ev_jet, std::uint32_t order, std::uint32_t dim)
{
    using std::isfinite;

    // Clear the vectors of detected events.
    // NOTE: do it here as this is always necessary,
    // regardless of issues with h.
    d_tes.clear();
    d_ntes.clear();

    if (!isfinite(h)) {
        // LCOV_EXCL_START
        get_logger()->warn("event detection skipped due to an invalid timestep value of {}", h);
        return;
        // LCOV_EXCL_STOP
    }

    if (h == 0) {
        // If the timestep is zero, skip event detection.
        return;
    }

    assert(order >= 2u);

    // Fetch a reference to the list of isolating intervals.
    auto &isol = get_isol<T>();

    // Fetch a reference to the wlist.
    auto &wl = get_wlist<T>();

    // Fetch the polynomial translation function.
    auto pt1 = get_poly_translator_1<T>(order);

    // Helper to run event detection on a vector of events
    // (terminal or not). 'out' is the vector of detected
    // events, 'ev_vec' the input vector of events to detect.
    auto run_detection = [&](auto &out, const auto &ev_vec) {
        // Fetch the event type.
        using ev_type = typename uncvref_t<decltype(ev_vec)>::value_type;

        for (std::uint32_t i = 0; i < ev_vec.size(); ++i) {
            // Clear out the list of isolating intervals.
            isol.clear();

            // Reset the working list.
            wl.clear();

            // Extract the pointer to the Taylor polynomial for the
            // current event.
            const auto ptr
                = ev_jet.data() + (i + dim + (is_terminal_event_v<ev_type> ? 0u : tes.size())) * (order + 1u);

            // Helper to add a detected event to out.
            // NOTE: the root here is expected to be already rescaled
            // to the [0, h) range.
            auto add_d_event = [&](T root) {
                // NOTE: we do one last check on the root in order to
                // avoid non-finite event times. This guarantees that
                // sorting the events by time is safe.
                if (!isfinite(root)) {
                    // LCOV_EXCL_START
                    get_logger()->warn("polynomial root finding produced a non-finite root of {} - skipping the event",
                                       root);
                    return;
                    // LCOV_EXCL_STOP
                }

                [[maybe_unused]] const bool has_multi_roots = [&]() {
                    if constexpr (is_terminal_event_v<ev_type>) {
                        // Establish the cooldown time.
                        // NOTE: this is the same logic that is
                        // employed in taylor.cpp to assign a cooldown
                        // to a detected terminal event.
                        const auto cd
                            = (ev_vec[i].get_cooldown() >= 0) ? ev_vec[i].get_cooldown() : taylor_deduce_cooldown(h);

                        // Evaluate the polynomial at the cooldown boundaries.
                        const auto e1 = poly_eval(ptr, root + cd, order);
                        const auto e2 = poly_eval(ptr, root - cd, order);

                        // We detect multiple roots within the cooldown
                        // if the signs of e1 and e2 are equal.
                        return (e1 > 0) == (e2 > 0);
                    } else {
                        return false;
                    }
                }();

                // Fetch and cache the event direction.
                const auto dir = ev_vec[i].get_direction();

                if (dir == event_direction::any) {
                    // If the event direction does not
                    // matter, just add it.
                    if constexpr (is_terminal_event_v<ev_type>) {
                        out.emplace_back(i, root, has_multi_roots);
                    } else {
                        out.emplace_back(i, root);
                    }
                } else {
                    // Otherwise, we need to compute the derivative
                    // and record the event only if its direction
                    // matches the sign of the derivative.
                    const auto der = poly_eval_1(ptr, root, order);

                    if ((der > 0 && dir == event_direction::positive)
                        || (der < 0 && dir == event_direction::negative)) {
                        if constexpr (is_terminal_event_v<ev_type>) {
                            out.emplace_back(i, root, has_multi_roots);
                        } else {
                            out.emplace_back(i, root);
                        }
                    }
                }
            };

            // NOTE: if we are dealing with a terminal event on cooldown,
            // we will need to ignore roots within the cooldown period.
            // lb_offset is the value in the original [0, 1) range corresponding
            // to the end of the cooldown.
            const auto lb_offset = [&]() {
                if constexpr (is_terminal_event_v<ev_type>) {
                    if (cooldowns[i]) {
                        using std::abs;

                        // NOTE: need to distinguish between forward
                        // and backward integration.
                        if (h >= 0) {
                            return (cooldowns[i]->second - cooldowns[i]->first) / abs(h);
                        } else {
                            return (cooldowns[i]->second + cooldowns[i]->first) / abs(h);
                        }
                    }
                }

                // NOTE: we end up here if the event is not terminal
                // or not on cooldown.
                return T(0);
            }();

            if (lb_offset >= 1) {
                // LCOV_EXCL_START
                // NOTE: the whole integration range is in the cooldown range,
                // move to the next event.
                SPDLOG_LOGGER_DEBUG(
                    get_logger(),
                    "the integration timestep falls within the cooldown range for the terminal event {}, skipping", i);
                continue;
                // LCOV_EXCL_STOP
            }

            // Rescale it so that the range [0, h)
            // becomes [0, 1).
            pwrap<T> tmp(order);
            poly_rescale(tmp.v.data(), ptr, h, order);

            // Place the first element in the working list.
            wl.emplace_back(0, 1, std::move(tmp));

#if !defined(NDEBUG)
            auto max_wl_size = wl.size();
            auto max_isol_size = isol.size();
#endif

            // Flag to signal that the do-while loop below failed.
            bool loop_failed = false;

            do {
                // Fetch the current interval and polynomial from the working list.
                // NOTE: q(x) is the transformed polynomial whose roots in the x range [0, 1) we will
                // be looking for. lb and ub represent what 0 and 1 correspond to in the *original*
                // [0, 1) range.
                auto [lb, ub, q] = std::move(wl.back());
                wl.pop_back();

                // Check for an event at the lower bound, which occurs
                // if the constant term of the polynomial is zero. We also
                // check for finiteness of all the other coefficients, otherwise
                // we cannot really claim to have detected an event.
                // When we do proper root finding below, the
                // algorithm should be able to detect non-finite
                // polynomials.
                if (q.v[0] == T(0) // LCOV_EXCL_LINE
                    && std::all_of(q.v.data() + 1, q.v.data() + 1 + order, [](const auto &x) { return isfinite(x); })) {
                    // NOTE: we will have to skip the event if we are dealing
                    // with a terminal event on cooldown and the lower bound
                    // falls within the cooldown time.
                    bool skip_event = false;
                    if constexpr (is_terminal_event_v<ev_type>) {
                        if (lb < lb_offset) {
                            SPDLOG_LOGGER_DEBUG(get_logger(),
                                                "terminal event {} detected at the beginning of an isolating interval "
                                                "is subject to cooldown, ignoring",
                                                i);
                            skip_event = true;
                        }
                    }

                    if (!skip_event) {
                        // NOTE: the original range had been rescaled wrt to h.
                        // Thus, we need to rescale back when adding the detected
                        // event.
                        add_d_event(lb * h);
                    }
                }

                // Reverse it.
                pwrap<T> tmp1(order);
                std::copy(q.v.rbegin(), q.v.rend(), tmp1.v.data());

                // Translate it.
                pwrap<T> tmp2(order);
                pt1(tmp2.v.data(), tmp1.v.data());

                // Count the sign changes.
                const auto n_sc = count_sign_changes(tmp2.v.data(), order);

                if (n_sc == 1u) {
                    // Found isolating interval, add it to isol.
                    isol.emplace_back(lb, ub);
                } else if (n_sc > 1u) {
                    // No isolating interval found, bisect.

                    // First we transform q into 2**n * q(x/2) and store the result
                    // into tmp1.
                    poly_rescale_p2(tmp1.v.data(), q.v.data(), order);
                    // Then we take tmp1 and translate it to produce 2**n * q((x+1)/2).
                    pt1(tmp2.v.data(), tmp1.v.data());

                    // Finally we add tmp1 and tmp2 to the working list.
                    const auto mid = (lb + ub) / 2;
                    // NOTE: don't add the lower range if it falls
                    // entirely within the cooldown range.
                    if (lb_offset < mid) {
                        wl.emplace_back(lb, mid, std::move(tmp1));
                    } else {
                        // LCOV_EXCL_START
                        SPDLOG_LOGGER_DEBUG(
                            get_logger(),
                            "ignoring lower interval in a bisection that would fall entirely in the cooldown period");
                        // LCOV_EXCL_STOP
                    }
                    wl.emplace_back(mid, ub, std::move(tmp2));
                }

#if !defined(NDEBUG)
                max_wl_size = std::max(max_wl_size, wl.size());
                max_isol_size = std::max(max_isol_size, isol.size());
#endif

                // We want to put limits in order to avoid an endless loop when the algorithm fails.
                // The first check is on the working list size and it is based
                // on heuristic observation of the algorithm's behaviour in pathological
                // cases. The second check is that we cannot possibly find more isolating
                // intervals than the degree of the polynomial.
                if (wl.size() > 250u || isol.size() > order) {
                    // LCOV_EXCL_START
                    get_logger()->warn(
                        "the polynomial root isolation algorithm failed during event detection: the working "
                        "list size is {} and the number of isolating intervals is {}",
                        wl.size(), isol.size());

                    loop_failed = true;

                    break;
                    // LCOV_EXCL_STOP
                }

            } while (!wl.empty());

#if !defined(NDEBUG)
            SPDLOG_LOGGER_DEBUG(get_logger(), "max working list size: {}", max_wl_size);
            SPDLOG_LOGGER_DEBUG(get_logger(), "max isol list size   : {}", max_isol_size);
#endif

            if (isol.empty() || loop_failed) {
                // Don't do root finding for this event if the loop failed,
                // or if the list of isolating intervals is empty. Just
                // move to the next event.
                continue;
            }

            // Reconstruct a version of the original event polynomial
            // in which the range [0, h) is rescaled to [0, 1). We need
            // to do root finding on the rescaled polynomial because the
            // isolating intervals are also rescaled to [0, 1).
            pwrap<T> tmp1(order);
            poly_rescale(tmp1.v.data(), ptr, h, order);

            // Run the root finding in the isolating intervals.
            for (auto &[lb, ub] : isol) {
                if constexpr (is_terminal_event_v<ev_type>) {
                    // NOTE: if we are dealing with a terminal event
                    // subject to cooldown, we need to ensure that
                    // we don't look for roots before the cooldown has expired.
                    if (lb < lb_offset) {
                        // Make sure we move lb past the cooldown.
                        lb = lb_offset;

                        // NOTE: this should be ensured by the fact that
                        // we ensure above (lb_offset < mid) that we don't
                        // end up with an invalid interval.
                        assert(lb < ub);

                        // Check if the interval still contains a zero.
                        const auto f_lb = poly_eval(tmp1.v.data(), lb, order);
                        const auto f_ub = poly_eval(tmp1.v.data(), ub, order);

                        if (!(f_lb * f_ub < 0)) {
                            SPDLOG_LOGGER_DEBUG(get_logger(), "terminal event {} is subject to cooldown, ignoring", i);
                            continue;
                        }
                    }
                }

                // Run the root finding.
                const auto [root, cflag] = bracketed_root_find(tmp1, order, lb, ub);

                if (cflag == 0) {
                    // Root finding finished successfully, record the event.
                    // The found root needs to be rescaled by h.
                    add_d_event(root * h);
                } else {
                    // Root finding encountered some issue. Ignore the
                    // event and log the issue.
                    if (cflag == -1) {
                        // LCOV_EXCL_START
                        get_logger()->warn(
                            "polynomial root finding during event detection failed due to too many iterations");
                        // LCOV_EXCL_STOP
                    } else {
                        get_logger()->warn(
                            "polynomial root finding during event detection returned a nonzero errno with message '{}'",
                            std::strerror(cflag));
                    }
                }
            }
        }
    };

    run_detection(d_tes, tes);
    run_detection(d_ntes, ntes);
}

} // namespace

template <>
void taylor_detect_events(std::vector<std::tuple<std::uint32_t, double, bool>> &d_tes,
                          std::vector<std::tuple<std::uint32_t, double>> &d_ntes,
                          const std::vector<t_event<double>> &tes, const std::vector<nt_event<double>> &ntes,
                          const std::vector<std::optional<std::pair<double, double>>> &cooldowns, double h,
                          const std::vector<double> &ev_jet, std::uint32_t order, std::uint32_t dim)
{
    taylor_detect_events_impl(d_tes, d_ntes, tes, ntes, cooldowns, h, ev_jet, order, dim);
}

template <>
void taylor_detect_events(std::vector<std::tuple<std::uint32_t, long double, bool>> &d_tes,
                          std::vector<std::tuple<std::uint32_t, long double>> &d_ntes,
                          const std::vector<t_event<long double>> &tes, const std::vector<nt_event<long double>> &ntes,
                          const std::vector<std::optional<std::pair<long double, long double>>> &cooldowns,
                          long double h, const std::vector<long double> &ev_jet, std::uint32_t order, std::uint32_t dim)
{
    taylor_detect_events_impl(d_tes, d_ntes, tes, ntes, cooldowns, h, ev_jet, order, dim);
}

#if defined(HEYOKA_HAVE_REAL128)

template <>
void taylor_detect_events(std::vector<std::tuple<std::uint32_t, mppp::real128, bool>> &d_tes,
                          std::vector<std::tuple<std::uint32_t, mppp::real128>> &d_ntes,
                          const std::vector<t_event<mppp::real128>> &tes,
                          const std::vector<nt_event<mppp::real128>> &ntes,
                          const std::vector<std::optional<std::pair<mppp::real128, mppp::real128>>> &cooldowns,
                          mppp::real128 h, const std::vector<mppp::real128> &ev_jet, std::uint32_t order,
                          std::uint32_t dim)
{
    taylor_detect_events_impl(d_tes, d_ntes, tes, ntes, cooldowns, h, ev_jet, order, dim);
}

#endif

namespace
{

// Helper to automatically deduce the cooldown
// for a terminal event which was detected within
// a timestep of size h.
// NOTE: the idea here is that event detection
// yielded an event time accurate to about 4*eps
// relative to the timestep size. Thus, we use as
// cooldown time a small multiple of that accuracy.
template <typename T>
T taylor_deduce_cooldown_impl(T h)
{
    using std::abs;

    const auto abs_h = abs(h);
    constexpr auto delta = 12 * std::numeric_limits<T>::epsilon();

    // NOTE: in order to avoid issues with small timesteps
    // or zero timestep, we use delta as a relative value
    // if abs_h >= 1, absolute otherwise.
    return abs_h >= 1 ? (abs_h * delta) : delta;
}

} // namespace

template <>
double taylor_deduce_cooldown(double h)
{
    return taylor_deduce_cooldown_impl(h);
}

template <>
long double taylor_deduce_cooldown(long double h)
{
    return taylor_deduce_cooldown_impl(h);
}

#if defined(HEYOKA_HAVE_REAL128)

template <>
mppp::real128 taylor_deduce_cooldown(mppp::real128 h)
{
    return taylor_deduce_cooldown_impl(h);
}

#endif

} // namespace heyoka::detail