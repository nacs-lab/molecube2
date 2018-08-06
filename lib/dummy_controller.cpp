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

#include "dummy_controller.h"

#include <nacs-utils/timer.h>

namespace Molecube {

DummyController::DummyController()
    : m_cur_t(getTime())
{
}

bool DummyController::concurrent_set(ReqOP op, uint32_t operand,
                                     bool is_override, uint32_t val)
{
    // Match the implementation in Controller.
    if (op != TTL)
        return false;
    if (!is_override)
        return false;
    if (operand == 0) {
        m_ttl_ovrlo.store(val, std::memory_order_relaxed);
        return true;
    }
    else if (operand == 1) {
        m_ttl_ovrhi.store(val, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool DummyController::concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                                     uint32_t &val)
{
    if (op == Clock) {
        val = m_clock.load(std::memory_order_relaxed);
        return false;
    }
    if (op != TTL)
        return false;
    if (!is_override) {
        if (operand != 0)
            return false;
        val = (m_ttl.load(std::memory_order_relaxed) |
               m_ttl_ovrhi.load(std::memory_order_relaxed)) &
            ~m_ttl_ovrlo.load(std::memory_order_relaxed);
        return true;
    }
    if (operand == 0) {
        val = m_ttl_ovrlo.load(std::memory_order_relaxed);
        return true;
    }
    else if (operand == 1) {
        val = m_ttl_ovrhi.load(std::memory_order_relaxed);
        return true;
    }
    return false;
}

}
