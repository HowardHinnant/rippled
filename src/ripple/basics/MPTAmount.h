//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_BASICS_INTEGRALAMOUNT_H_INCLUDED
#define RIPPLE_BASICS_INTEGRALAMOUNT_H_INCLUDED

#include <ripple/basics/contract.h>
#include <ripple/basics/safe_cast.h>
#include <ripple/beast/utility/Zero.h>
#include <ripple/json/json_value.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <boost/operators.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

namespace ripple {

class MPTAmount : private boost::totally_ordered<MPTAmount>,
                  private boost::additive<MPTAmount>,
                  private boost::equality_comparable<MPTAmount, std::int64_t>,
                  private boost::additive<MPTAmount, std::int64_t>
{
public:
    using mpt_type = std::int64_t;

protected:
    mpt_type mpt_;

public:
    MPTAmount() = default;
    constexpr MPTAmount(MPTAmount const& other) = default;
    constexpr MPTAmount&
    operator=(MPTAmount const& other) = default;

    constexpr MPTAmount(beast::Zero) : mpt_(0)
    {
    }

    constexpr MPTAmount(mpt_type value) : mpt_(value)
    {
    }

    constexpr MPTAmount& operator=(beast::Zero)
    {
        mpt_ = 0;
        return *this;
    }

    MPTAmount&
    operator+=(MPTAmount const& other)
    {
        mpt_ += other.mpt();
        return *this;
    }

    MPTAmount&
    operator-=(MPTAmount const& other)
    {
        mpt_ -= other.mpt();
        return *this;
    }

    MPTAmount
    operator-() const
    {
        return MPTAmount{-mpt_};
    }

    bool
    operator==(MPTAmount const& other) const
    {
        return mpt_ == other.mpt_;
    }

    bool
    operator==(mpt_type other) const
    {
        return mpt_ == other;
    }

    bool
    operator<(MPTAmount const& other) const
    {
        return mpt_ < other.mpt_;
    }

    /** Returns true if the amount is not zero */
    explicit constexpr operator bool() const noexcept
    {
        return mpt_ != 0;
    }

    /** Return the sign of the amount */
    constexpr int
    signum() const noexcept
    {
        return (mpt_ < 0) ? -1 : (mpt_ ? 1 : 0);
    }

    Json::Value
    jsonClipped() const
    {
        static_assert(
            std::is_signed_v<mpt_type> && std::is_integral_v<mpt_type>,
            "Expected MPTAmount to be a signed integral type");

        constexpr auto min = std::numeric_limits<Json::Int>::min();
        constexpr auto max = std::numeric_limits<Json::Int>::max();

        if (mpt_ < min)
            return min;
        if (mpt_ > max)
            return max;
        return static_cast<Json::Int>(mpt_);
    }

    /** Returns the underlying value. Code SHOULD NOT call this
        function unless the type has been abstracted away,
        e.g. in a templated function.
    */
    constexpr mpt_type
    mpt() const
    {
        return mpt_;
    }

    friend std::istream&
    operator>>(std::istream& s, MPTAmount& val)
    {
        s >> val.mpt_;
        return s;
    }

    static MPTAmount
    minPositiveAmount()
    {
        return MPTAmount{1};
    }
};

// Output MPTAmount as just the value.
template <class Char, class Traits>
std::basic_ostream<Char, Traits>&
operator<<(std::basic_ostream<Char, Traits>& os, const MPTAmount& q)
{
    return os << q.mpt();
}

inline std::string
to_string(MPTAmount const& amount)
{
    return std::to_string(amount.mpt());
}

inline MPTAmount
mulRatio(
    MPTAmount const& amt,
    std::uint32_t num,
    std::uint32_t den,
    bool roundUp)
{
    using namespace boost::multiprecision;

    if (!den)
        Throw<std::runtime_error>("division by zero");

    int128_t const amt128(amt.mpt());
    auto const neg = amt.mpt() < 0;
    auto const m = amt128 * num;
    auto r = m / den;
    if (m % den)
    {
        if (!neg && roundUp)
            r += 1;
        if (neg && !roundUp)
            r -= 1;
    }
    if (r > std::numeric_limits<MPTAmount::mpt_type>::max())
        Throw<std::overflow_error>("XRP mulRatio overflow");
    return MPTAmount(r.convert_to<MPTAmount::mpt_type>());
}

}  // namespace ripple

#endif  // RIPPLE_BASICS_INTEGRALAMOUNT_H_INCLUDED
