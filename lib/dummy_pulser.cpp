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

NACS_PROTECTED() void DummyPulser::add_cmd(OP op, bool timing, uint32_t v1, uint32_t v2)
{
    Cmd cmd{op, timing, std::chrono::steady_clock::now(), v1, v2};
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    while (m_cmds.size() >= 4096) {
        if (m_hold) {
            m_hold = false;
            m_release_time = std::chrono::steady_clock::now();
        }
        forward_time(true, lock);
    }
    m_cmds.push(cmd);
    m_cmds_empty.store(false, std::memory_order_release);
}

NACS_PROTECTED() void DummyPulser::release_hold()
{
    // Protecting access to `m_release_time` and `m_hold`
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    if (!m_hold)
        return;
    m_hold = false;
    m_release_time = std::chrono::steady_clock::now();
}

NACS_PROTECTED() void DummyPulser::set_hold()
{
    // Protecting access to `m_hold`
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    if (m_hold)
        return;
    forward_time(false, lock);
    m_hold = true;
}

NACS_PROTECTED() void DummyPulser::toggle_init()
{
    if (!m_cmds_empty.load(std::memory_order_acquire))
        throw std::runtime_error("Command stream not empty during init.");
    m_timing_ok.store(true, std::memory_order_release);
}

NACS_PROTECTED() void DummyPulser::forward_time(bool block, std::unique_lock<std::mutex> &lock)
{
    // TODO
    // hold
    // timing_ok
}

NACS_INTERNAL uint32_t DummyPulser::run_cmd(const Cmd &cmd)
{
    switch (cmd.op) {
    case OP::TTL:
        m_ttl.store(cmd.v2, std::memory_order_release);
        return cmd.v1;
    case OP::Clock:
        m_clock.store(uint8_t(cmd.v1), std::memory_order_release);
        return 5;
    case OP::DAC:
        return 45;
    case OP::Wait:
        return cmd.v1;
    case OP::ClearErr:
        m_timing_ok.store(true, std::memory_order_release);
        return 5;
    case OP::DDSSetFreq:
        m_dds[cmd.v1].freq = cmd.v2;
        return 50;
    case OP::DDSSetAmp:
        m_dds[cmd.v1].amp = uint16_t(cmd.v2);
        return 50;
    case OP::DDSSetPhase:
        m_dds[cmd.v1].phase = uint16_t(cmd.v2);
        return 50;
    case OP::DDSReset:
        m_dds[cmd.v1].init = false;
        m_dds[cmd.v1].amp = 0;
        m_dds[cmd.v1].phase = 0;
        m_dds[cmd.v1].freq = 0;
        return 50;
    case OP::LoopBack:
        add_result(cmd.v1);
        return 5;
    case OP::DDSGetFreq:
        add_result(m_dds[cmd.v1].freq);
        return 50;
    case OP::DDSGetAmp:
        add_result(m_dds[cmd.v1].amp);
        return 50;
    case OP::DDSGetPhase:
        add_result(m_dds[cmd.v1].phase);
        return 50;
    default:
        throw std::runtime_error("Invalid command.");
    }
}

}
