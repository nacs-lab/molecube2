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

#include "dummy_pulser.h"

#include <stdexcept>
#include <thread>

namespace Molecube {

NACS_PROTECTED() DummyPulser::DummyPulser()
{
}

NACS_PROTECTED() void DummyPulser::init_dds(int chn)
{
    if (!dds_exists(chn))
        return;
    m_dds[chn].init = true;
}

NACS_PROTECTED() bool DummyPulser::check_dds(int chn, bool force)
{
    if (!dds_exists(chn))
        return true;
    if (force || !m_dds[chn].init) {
        init_dds(chn);
        return true;
    }
    return false;
}

NACS_PROTECTED() bool DummyPulser::dds_exists(int chn)
{
    return 0 <= chn && chn < NDDS;
}

NACS_PROTECTED() void DummyPulser::dump_dds(std::ostream &stm, int chn)
{
    stm << "*******************************" << std::endl;
    stm << "Dummy DDS board: " << chn << std::endl;
    stm << "*******************************" << std::endl;
}

NACS_PROTECTED() bool DummyPulser::try_get_result(uint32_t &res)
{
    if (m_results.empty()) {
        forward_time();
        if (m_results.empty()) {
            return false;
        }
    }
    res = m_results.front();
    m_results.pop();
    return true;
}

NACS_PROTECTED() uint32_t DummyPulser::get_result()
{
    uint32_t res;
    while (!try_get_result(res)) {
        if (m_cmds.empty())
            throw std::underflow_error("No result queued.");
        std::this_thread::yield();
    }
    return res;
}

NACS_INTERNAL void DummyPulser::add_result(uint32_t v)
{
    if (m_results.size() >= 32)
        throw std::overflow_error("Result number overflow.");
    m_results.push(v);
}

NACS_PROTECTED() void DummyPulser::add_cmd(Cmd cmd)
{
    // TODO
}

NACS_PROTECTED() void DummyPulser::forward_time()
{
    // TODO
}

}
