/*************************************************************************
 *   Copyright (c) 2018 - 2018 Yichao Yu <yyc1992@gmail.com>             *
 *                                                                       *
 *   This library is free software; you can redistribute it and/or       *
 *   modify it under the terms of the GNU Lesser General Public          *
 *   License as published by the Free Software Foundation; either        *
 *   version 3.0 of the License, or (at your option) any later version.  *
 *                                                                       *
 *   This library is distributed in the hope that it will be useful,     *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of      *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU    *
 *   Lesser General Public License for more details.                     *
 *                                                                       *
 *   You should have received a copy of the GNU Lesser General Public    *
 *   License along with this library. If not,                            *
 *   see <http://www.gnu.org/licenses/>.                                 *
 *************************************************************************/

#ifndef LIBMOLECUBE_PULSER_COMMON_H
#define LIBMOLECUBE_PULSER_COMMON_H

#include <stdint.h>

#include <string>

namespace Molecube {

static constexpr int NUM_TTL_BANKS = 8;
struct pulser_version_t {
    uint32_t major;
    uint32_t minor;
    bool check_compatible(const pulser_version_t &ver) const
    {
        return major == ver.major && minor >= ver.minor;
    }
    bool check_at_least(const pulser_version_t &ver) const
    {
        if (major > ver.major)
            return true;
        if (major < ver.major)
            return false;
        return minor >= ver.minor;
    }
};

static inline std::string to_string(const pulser_version_t &ver)
{
    return std::to_string(ver.major) + "." + std::to_string(ver.minor);
}

}

#endif
