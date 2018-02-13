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

#ifndef LIBMOLECUBE_CTRL_IFACE_H
#define LIBMOLECUBE_CTRL_IFACE_H

#include <functional>
#include <nacs-utils/utils.h>

namespace Molecube {

using namespace NaCs;

namespace DDS {

enum Type : uint8_t {
    Freq,
    Amp,
    Phase
};

struct __attribute__((__packed__)) Info {
    uint8_t typ: 2;
    uint8_t id: 6;
    uint32_t val;
};

}

}

#endif
