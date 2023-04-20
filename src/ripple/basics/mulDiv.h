//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2015 Ripple Labs Inc.

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

#ifndef RIPPLE_BASICS_MULDIV_H_INCLUDED
#define RIPPLE_BASICS_MULDIV_H_INCLUDED

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace ripple {
auto constexpr muldiv_max = std::numeric_limits<std::uint64_t>::max();

/** Return value*mul/div accurately.
    Computes the result of the multiplication and division in
    a single step, avoiding overflow and retaining precision.
    Throws:
        None
    Returns:
        `std::optional`:
            If the calculation overflows, this function returns std::nullopt
            otherwise, the result of the calculation is returned.
*/
std::optional<std::uint64_t>
mulDiv(std::uint64_t value, std::uint64_t mul, std::uint64_t div);

}  // namespace ripple

#endif
