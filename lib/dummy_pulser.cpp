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

#include <nacs-seq/seq.h>

#include <stdexcept>
#include <thread>

namespace Molecube {

NACS_EXPORT() DummyPulser::DummyPulser()
{
}

NACS_EXPORT() void DummyPulser::init_dds(int chn)
{
    const int cmd_count = 7 + 46;
    m_inst_word_count.fetch_add(cmd_count * 2, std::memory_order_relaxed);

    if (!dds_exists_internal(chn))
        return;
    dds_reset<false>(chn);
    m_dds[chn].init = true;
}

NACS_EXPORT() bool DummyPulser::check_dds(int chn, bool force)
{
    if (!force) {
        m_inst_word_count.fetch_add(2, std::memory_order_relaxed);
        if (dds_exists_internal(chn) && m_dds[chn].init) {
            return false;
        }
    }
    init_dds(chn);
    return true;
}

NACS_EXPORT() bool DummyPulser::dds_exists(int chn)
{
    const int cmd_count = 4;
    m_inst_word_count.fetch_add(cmd_count * 2, std::memory_order_relaxed);
    return dds_exists_internal(chn);
}

NACS_EXPORT() void DummyPulser::dump_dds(std::ostream &stm, int chn)
{
    stm << "*******************************" << std::endl;
    stm << "Dummy DDS board: " << chn << std::endl;
    stm << "*******************************" << std::endl;
    const int cmd_count = 32;
    m_inst_word_count.fetch_add(cmd_count * 2, std::memory_order_relaxed);
}

NACS_EXPORT() bool DummyPulser::try_get_result(uint32_t &res)
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

NACS_EXPORT() uint32_t DummyPulser::get_result()
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

NACS_EXPORT() void DummyPulser::add_cmd(OP op, bool timing, uint32_t v1, uint32_t v2)
{
    Cmd cmd{op, timing, std::chrono::steady_clock::now(), v1, v2};
    m_inst_word_count.fetch_add(2, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    while (m_cmds.size() >= 4096) {
        if (!m_force_release) {
            m_force_release = true;
            m_release_time = std::chrono::steady_clock::now();
        }
        forward_time(true, lock);
    }
    m_cmds.push(cmd);
    m_cmds_empty.store(false, std::memory_order_release);
}

NACS_EXPORT() void DummyPulser::release_hold()
{
    // Protecting access to `m_release_time` and `m_hold`
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    if (!m_hold)
        return;
    if (!m_force_release)
        m_release_time = std::chrono::steady_clock::now();
    m_hold = false;
}

NACS_EXPORT() void DummyPulser::set_hold()
{
    // Protecting access to `m_hold`
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    if (m_hold)
        return;
    if (!m_force_release)
        forward_time(false, lock);
    m_hold = true;
}

NACS_EXPORT() void DummyPulser::toggle_init()
{
    if (!m_cmds_empty.load(std::memory_order_acquire))
        throw std::runtime_error("Command stream not empty during init.");
    m_force_release = false;
    m_timing_ok.store(true, std::memory_order_release);
    m_timing_check.store(false, std::memory_order_release);

    m_inst_word_count.store(0, std::memory_order_relaxed);
    m_inst_count.store(0, std::memory_order_relaxed);
    m_ttl_count.store(0, std::memory_order_relaxed);
    m_dds_count.store(0, std::memory_order_relaxed);
    m_wait_count.store(0, std::memory_order_relaxed);
    m_clear_error_count.store(0, std::memory_order_relaxed);
    m_loopback_count.store(0, std::memory_order_relaxed);
    m_clock_count.store(0, std::memory_order_relaxed);
    m_spi_count.store(0, std::memory_order_relaxed);
    m_underflow_cycle.store(0, std::memory_order_relaxed);
    m_inst_cycle.store(0, std::memory_order_relaxed);
    m_ttl_cycle.store(0, std::memory_order_relaxed);
    m_wait_cycle.store(0, std::memory_order_relaxed);
}

NACS_EXPORT() void DummyPulser::forward_time(bool block, std::unique_lock<std::mutex>&)
{
    if (m_cmds.empty() || (m_hold && !m_force_release)) {
        if (block)
            throw std::runtime_error("Waiting for command queue without releasing hold.");
        return;
    }
    do {
        auto cur_t = std::chrono::steady_clock::now();
        if (cur_t < m_release_time) {
            std::this_thread::sleep_until(m_release_time);
            cur_t = std::chrono::steady_clock::now();
        }
        if (run_past_cmds(cur_t))
            block = false;
    } while (block);
}

NACS_INTERNAL bool DummyPulser::run_past_cmds(time_point_t cur_t)
{
    if (m_hold && !m_force_release)
        return false;
    bool cmd_run = false;
    while (!m_cmds.empty()) {
        auto &cmd = m_cmds.front();
        auto cmdt = cmd.t;
        auto startt = m_release_time;
        if (cmdt > startt) {
            if (m_timing_check.load(std::memory_order_acquire)) {
                m_timing_ok.store(false, std::memory_order_release);
                auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(cmdt - startt);
                m_underflow_cycle.fetch_add(uint32_t(dt.count() / 10 + 1),
                                            std::memory_order_relaxed);
            }
            startt = cmdt;
            assert(cmdt <= cur_t);
        }
        else if (startt > cur_t) {
            return cmd_run;
        }
        cmd_run = true;
        m_timing_check.store(cmd.timing, std::memory_order_release);
        auto steps = run_cmd(cmd);
        m_inst_cycle.fetch_add(steps, std::memory_order_relaxed);
        m_release_time = startt + std::chrono::nanoseconds(steps * 10);
        m_cmds.pop();
    }
    m_cmds_empty.store(true, std::memory_order_release);
    return cmd_run;
}

NACS_INTERNAL uint32_t DummyPulser::run_cmd(const Cmd &cmd)
{
    m_inst_count.fetch_add(1, std::memory_order_relaxed);
    switch (cmd.op) {
    case OP::TTL:
        m_ttl_count.fetch_add(1, std::memory_order_relaxed);
        m_ttl.store(cmd.v2, std::memory_order_release);
        m_ttl_cycle.fetch_add(cmd.v1, std::memory_order_relaxed);
        return cmd.v1;
    case OP::Clock:
        m_clock_count.fetch_add(1, std::memory_order_relaxed);
        m_clock.store(uint8_t(cmd.v1), std::memory_order_release);
        return Seq::PulseTime::Clock;
    case OP::DAC:
        m_spi_count.fetch_add(1, std::memory_order_relaxed);
        return Seq::PulseTime::DAC;
    case OP::Wait:
        m_wait_count.fetch_add(1, std::memory_order_relaxed);
        m_wait_cycle.fetch_add(cmd.v1, std::memory_order_relaxed);
        return cmd.v1;
    case OP::ClearErr:
        m_clear_error_count.fetch_add(1, std::memory_order_relaxed);
        m_timing_ok.store(true, std::memory_order_release);
        m_underflow_cycle.store(0, std::memory_order_relaxed);
        return Seq::PulseTime::Clear;
    case OP::DDSSetFreq:
        m_dds_count.fetch_add(1, std::memory_order_relaxed);
        m_dds[cmd.v1].freq = cmd.v2;
        return Seq::PulseTime::DDSFreq;
    case OP::DDSSetAmp:
        m_dds_count.fetch_add(1, std::memory_order_relaxed);
        m_dds[cmd.v1].amp = uint16_t(cmd.v2);
        return Seq::PulseTime::DDSAmp;
    case OP::DDSSetPhase:
        m_dds_count.fetch_add(1, std::memory_order_relaxed);
        m_dds[cmd.v1].phase = uint16_t(cmd.v2);
        return Seq::PulseTime::DDSPhase;
    case OP::DDSReset:
        m_dds_count.fetch_add(1, std::memory_order_relaxed);
        m_dds[cmd.v1].amp = 0;
        m_dds[cmd.v1].phase = 0;
        m_dds[cmd.v1].freq = 0;
        return Seq::PulseTime::DDSReset;
    case OP::LoopBack:
        m_loopback_count.fetch_add(1, std::memory_order_relaxed);
        add_result(cmd.v1);
        return Seq::PulseTime::LoopBack;
    case OP::DDSGetFreq:
        m_dds_count.fetch_add(1, std::memory_order_relaxed);
        add_result(m_dds[cmd.v1].freq);
        return Seq::PulseTime::DDSFreq;
    case OP::DDSGetAmp:
        m_dds_count.fetch_add(1, std::memory_order_relaxed);
        add_result(m_dds[cmd.v1].amp);
        return Seq::PulseTime::DDSAmp;
    case OP::DDSGetPhase:
        m_dds_count.fetch_add(1, std::memory_order_relaxed);
        add_result(m_dds[cmd.v1].phase);
        return Seq::PulseTime::DDSPhase;
    default:
        throw std::runtime_error("Invalid command.");
    }
}

#define _NACS_EXPORT NACS_EXPORT() // Somehow the () really messes up emacs indent...

_NACS_EXPORT
DummyPulser::DummyPulser(DummyPulser &&o)
    : m_ttl_hi(o.m_ttl_hi.load(std::memory_order_relaxed)),
      m_ttl_lo(o.m_ttl_lo.load(std::memory_order_relaxed)),
      m_ttl(o.m_ttl.load(std::memory_order_relaxed)),
      m_clock(o.m_clock.load(std::memory_order_relaxed)),
      m_cmds_empty(o.m_cmds_empty.load(std::memory_order_relaxed)),
      m_timing_ok(o.m_timing_ok.load(std::memory_order_relaxed)),
      m_timing_check(o.m_timing_check.load(std::memory_order_relaxed)),
      m_results(std::move(o.m_results)),
      m_cmds(std::move(o.m_cmds)),
      m_hold(o.m_hold),
      m_force_release(o.m_force_release),
      m_dds(o.m_dds),
      m_release_time(o.m_release_time)
{
}

}
