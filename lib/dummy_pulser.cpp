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

#include <nacs-seq/zynq/pulse_time.h>
#include <nacs-utils/number.h>

#include <atomic>
#include <bit>
#include <stdexcept>
#include <thread>

namespace Molecube {

NACS_EXPORT() DummyPulser::DummyPulser()
{
}

// The DDS functions below send the same commands as the ones in `Pulser`.

NACS_EXPORT() void DummyPulser::init_dds(int chn, bool)
{
    if (!dds_exists_internal(chn))
        return;
    dds_reset<false>(chn);
    // Calibration
    dds_set_2bytes<false>(chn, 0x0e, 0x0105);
    dds_set_2bytes<false>(chn, 0x0e, 0x0405);
    // Disable SYNC_CLK output
    dds_set_2bytes<false>(chn, 0x04, 0x0100);
    // Disable ramp, disable programmable modulus, enable profile mode
    dds_set_2bytes<false>(chn, 0x06, 0x0080);
    // Enable amplitude control (OSK)
    dds_set_2bytes<false>(chn, 0x0, 0x0308);
    // Zero-out all other memory, except the magic bytes.
    for (unsigned addr = 0x10;addr <= 0x68;addr += 4) {
        dds_set_4bytes<false>(chn, addr, addr == 0x64 ? magic_bytes : 0);
    }
}

NACS_EXPORT() bool DummyPulser::check_dds(int chn, bool force, bool dma)
{
    if (!force) {
        dds_get_4bytes<false>(chn, 0x64);
        if (get_result() == magic_bytes) {
            return false;
        }
    }
    init_dds(chn, dma);
    return true;
}

NACS_EXPORT() bool DummyPulser::dds_exists(int chn)
{
    if (!dds_exists_internal(chn))
        return false;
    dds_set_2bytes<false>(chn, 0x68, 0);
    dds_get_2bytes<false>(chn, 0x68);
    dds_set_2bytes<false>(chn, 0x68, 1);
    dds_get_2bytes<false>(chn, 0x68);
    auto res0 = get_result();
    auto res1 = get_result();
    return res0 == 0 && res1 == 1;
}

NACS_EXPORT() void DummyPulser::dump_dds(std::ostream &stm, int chn)
{
    stm << "*******************************" << std::endl;
    for (unsigned addr = 0; addr + 3 <= 0x7f; addr += 4) {
        dds_get_4bytes<false>(chn, addr);
        if (uint32_t u = get_result()) {
            stm << "Dummy DDS board = " << std::dec << chn << ", addr = 0x"
                << std::hex << addr + 3 << "..." << addr
                << " = 0x" << u << std::dec << std::endl;
        }
    }
    stm << "*******************************" << std::endl;
}

NACS_EXPORT() bool DummyPulser::try_get_result(uint32_t &res)
{
    forward_time();
    if (m_results.empty())
        return false;
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

NACS_EXPORT() uint16_t DummyPulser::read_dds(int chn, uint8_t addr)
{
    assert(dds_exists_internal(chn));
    // Protecting access to `m_dds`
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    forward_time(false, lock);
    return m_dds_cache[chn][(addr & 0x7f) >> 1];
}

NACS_INTERNAL void DummyPulser::add_result(uint32_t v)
{
    if (m_results.size() >= max_result_count)
        v = 0;
    m_results.push(v);
}

NACS_EXPORT() void DummyPulser::add_cmd(OP op, bool timing, uint32_t v1, uint32_t v2)
{
    Cmd cmd{op, timing, std::chrono::steady_clock::now(), v1, v2};
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
}

NACS_EXPORT() void DummyPulser::forward_time(bool block, std::unique_lock<std::mutex> &locker)
{
    if (m_cmds.empty() || (m_hold && !m_force_release)) {
        if (block)
            throw std::runtime_error("Waiting for command queue without releasing hold.");
        return;
    }
    do {
        auto cur_t = std::chrono::steady_clock::now();
        if (cur_t < m_release_time) {
            locker.unlock();
            std::this_thread::sleep_until(m_release_time);
            locker.lock();
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
        m_release_time = startt + std::chrono::nanoseconds(steps * 10);
        m_cmds.pop();
    }
    m_cmds_empty.store(true, std::memory_order_release);
    return cmd_run;
}

NACS_INTERNAL uint32_t DummyPulser::run_cmd(const Cmd &cmd)
{
    switch (cmd.op) {
    case OP::TTL: {
        auto t = cmd.v1 & ((uint32_t(1) << 24) - 1);
        auto bank = cmd.v1 >> 24;
        m_ttl[bank].store(cmd.v2, std::memory_order_release);
        return t;
    }
    case OP::Clock:
        m_clock.store(uint8_t(cmd.v1), std::memory_order_release);
        return Seq::Zynq::PulseTime::Clock;
    case OP::DAC:
        return Seq::Zynq::PulseTime::DAC;
    case OP::Wait:
        return cmd.v1;
    case OP::ClearErr:
        m_timing_ok.store(true, std::memory_order_release);
        return Seq::Zynq::PulseTime::Clear;
    case OP::DDSSet2:
        write_dds_reg(cmd.v1, uint16_t(cmd.v2));
        return Seq::Zynq::PulseTime::DDSAmp;
    case OP::DDSSet4:
        write_dds_reg(cmd.v1, uint16_t(cmd.v2));
        write_dds_reg(cmd.v1 + (2 << 8), uint16_t(cmd.v2 >> 16));
        return Seq::Zynq::PulseTime::DDSFreq;
    case OP::DDSReset:
        // Only the registers of the DDS are reset, not the cache in the hardware.
        m_dds[cmd.v1 & 0xff].fill(0);
        return Seq::Zynq::PulseTime::DDSReset;
    case OP::LoopBack:
        add_result(cmd.v1);
        return Seq::Zynq::PulseTime::LoopBack;
    case OP::DDSGet2:
        add_result(read_dds_reg(cmd.v1));
        return Seq::Zynq::PulseTime::DDSAmp;
    case OP::DDSGet4: {
        uint32_t lo = read_dds_reg(cmd.v1);
        uint32_t hi = read_dds_reg(cmd.v1 + (2 << 8));
        add_result(lo | (hi << 16));
        return Seq::Zynq::PulseTime::DDSFreq;
    }
    default:
        throw std::runtime_error("Invalid command.");
    }
}

#define _NACS_EXPORT NACS_EXPORT() // Somehow the () really messes up emacs indent...

_NACS_EXPORT
DummyPulser::DummyPulser(DummyPulser &&o)
    : m_dma_count(o.m_dma_count.load(std::memory_order_relaxed)),
      m_dma_control(o.m_dma_control.load(std::memory_order_relaxed)),
      m_clock(o.m_clock.load(std::memory_order_relaxed)),
      m_cmds_empty(o.m_cmds_empty.load(std::memory_order_relaxed)),
      m_timing_ok(o.m_timing_ok.load(std::memory_order_relaxed)),
      m_timing_check(o.m_timing_check.load(std::memory_order_relaxed)),
      m_results(std::move(o.m_results)),
      m_cmds(std::move(o.m_cmds)),
      m_hold(o.m_hold),
      m_force_release(o.m_force_release),
      m_dds(o.m_dds),
      m_dds_cache(o.m_dds_cache),
      m_release_time(o.m_release_time),
      m_dma_dds_mask(o.m_dma_dds_mask)
{
    for (int i = 0; i < NUM_TTL_BANKS; i++) {
        m_ttl_hi[i].store(o.m_ttl_hi[i].load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
        m_ttl_lo[i].store(o.m_ttl_lo[i].load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
        m_ttl[i].store(o.m_ttl[i].load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        m_dma_ttl_mask[i].store(o.m_dma_ttl_mask[i].load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
    }
}

// Limit the total buffer size to mimic the size of the DMA buffer pool in the kernel.
static std::atomic<size_t> dma_buff_used{0};
static constexpr size_t dma_buff_pool_sz = 256 * 1024;

NACS_EXPORT() void *DummyPulser::alloc_buffer(size_t size)
{
    size = alignTo(size, 4096);
    auto used = dma_buff_used.load(std::memory_order_relaxed);
    do {
        if (used + size > dma_buff_pool_sz) {
            return nullptr;
        }
    } while (!dma_buff_used.compare_exchange_weak(used, used + size,
                                                  std::memory_order_relaxed));
    // Align the buffer so that it satisfies the DMA address requirement
    // (page aligned and not crossing a 1MB boundary).
    size_t align = std::bit_ceil(size);
    auto buff = aligned_alloc(align, alignTo(size, align));
    if (!buff)
        dma_buff_used.fetch_sub(size, std::memory_order_relaxed);
    return buff;
}
NACS_EXPORT() uintptr_t DummyPulser::buffer_addr(void *buff)
{
    return (uintptr_t)buff;
}
NACS_EXPORT() void DummyPulser::free_buffer(void *buff, size_t size)
{
    free(buff);
    dma_buff_used.fetch_sub(alignTo(size, 4096), std::memory_order_relaxed);
}

}
