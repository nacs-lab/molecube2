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

#include "controller.h"

#include <nacs-kernel/devctl.h>

namespace Molecube {

Controller::Controller()
    : m_addr(Kernel::mapPulseCtrl())
{
}

bool Controller::concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                                uint32_t val)
{
    if (op != TTL)
        return false;
    if (!is_override)
        return false;
    if (operand == 0) {
        set_ttl_lomask(val);
        return true;
    }
    else if (operand == 1) {
        set_ttl_himask(val);
        return true;
    }
    return false;
}

bool Controller::concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                                uint32_t &val)
{
    if (op == Clock) {
        val = cur_clock();
        return false;
    }
    if (op != TTL)
        return false;
    if (!is_override) {
        if (operand != 0)
            return false;
        val = (cur_ttl() | ttl_himask()) & ~ttl_lomask();
        return true;
    }
    if (operand == 0) {
        val = ttl_lomask();
        return true;
    }
    else if (operand == 1) {
        val = ttl_himask();
        return true;
    }
    return false;
}

}
