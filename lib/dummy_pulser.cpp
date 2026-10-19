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

#include <nacs-seq/zynq/dma.h>
#include <nacs-seq/zynq/pulse_time.h>
#include <nacs-utils/log.h>
#include <nacs-utils/mem.h>
#include <nacs-utils/number.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
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
    // The results may be added by the DMA from other threads.
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    forward_all(lock);
    if (m_results.empty())
        return false;
    res = m_results.front();
    m_results.pop();
    return true;
}

NACS_EXPORT() uint32_t DummyPulser::get_result()
{
    while (true) {
        {
            // The results and the commands must be checked together
            // since the commands may be run by other threads in the DMA mode.
            std::unique_lock<std::mutex> lock(m_cmds_lock);
            forward_all(lock);
            if (!m_results.empty()) {
                auto res = m_results.front();
                m_results.pop();
                return res;
            }
            // In the DMA mode, this waits for the commands to be run by the DMA.
            if (m_cmds.empty() && m_dma_cmds.empty()) {
                throw std::underflow_error("No result queued.");
            }
        }
        std::this_thread::yield();
    }
}

NACS_EXPORT() uint16_t DummyPulser::read_dds(int chn, uint8_t addr)
{
    assert(dds_exists_internal(chn));
    // Protecting access to `m_dds_cache`
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    forward_all(lock);
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
    if (m_dma_control.load(std::memory_order_relaxed) & 1) {
        // In the DMA mode, only the DDS and DAC commands are dispatched
        // (to be run in the long DMA waits).
        switch (op) {
        case OP::DAC:
        case OP::DDSSet2:
        case OP::DDSSet4:
        case OP::DDSReset:
        case OP::DDSGet2:
        case OP::DDSGet4:
            break;
        default:
            Log::warn("Dummy pulser: command %d ignored in the DMA mode.\n", int(op));
            return;
        }
        // Process the DMA up to when the command is sent so that the command
        // doesn't miss the long wait that it should run in.
        forward_dma(cmd.t);
        // The hardware stalls the bus until there's space in the queue.
        while (m_dma_cmds.size() >= dma_cmd_queue_size) {
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
            forward_all(lock);
        }
        m_dma_cmds.push(cmd);
        return;
    }
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
    m_trig_timeout.store(false, std::memory_order_release);
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

NACS_EXPORT() void DummyPulser::forward_all(std::unique_lock<std::mutex> &lock)
{
    forward_time(false, lock);
    forward_dma(std::chrono::steady_clock::now());
}

NACS_EXPORT() uint32_t DummyPulser::dma_status() const
{
    auto self = const_cast<DummyPulser*>(this);
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    self->forward_all(lock);
    uint32_t status = m_dma_count.load(std::memory_order_relaxed) & 0xff;
    if (m_dma_running)
        status |= 0x100;
    if (m_dma_underflow)
        status |= 0x200;
    if (m_dma_trig_timeout)
        status |= 0x400;
    if (m_dma_cmds.empty())
        status |= 0x800; // cmd_empty
    if (m_dma_cmds.size() >= dma_cmd_queue_size)
        status |= 0x1000; // cmd_full
    return status;
}

NACS_EXPORT() void DummyPulser::set_dma_control(uint32_t ctrl)
{
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    forward_all(lock);
    m_dma_control.store(ctrl & 1, std::memory_order_release);
}

NACS_EXPORT() void DummyPulser::start_dma(uintptr_t addr, uint16_t blocks, bool first)
{
    auto t = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(m_cmds_lock);
    // Process the DMA only up to when the transfer is started.
    forward_time(false, lock);
    forward_dma(t);
    if (!(m_dma_control.load(std::memory_order_relaxed) & 1)) {
        Log::warn("Dummy pulser: DMA transfer ignored without enabling the DMA mode.\n");
        return;
    }
    // Each block is 16 x 64 bits
    size_t sz = (size_t(blocks & 0x3ff) + 1) * 128;
    std::vector<uint8_t> data(sz);
    memcpy(data.data(), (const void*)addr, sz);
    m_dma_transfers.push_back(DMATransfer{t, first, std::move(data)});
    forward_dma(t);
}

NACS_INTERNAL void DummyPulser::read_dma_transfer()
{
    auto &transfer = m_dma_transfers.front();
    if (transfer.first) {
        // The status of the previous sequence is cleared by the first transfer.
        m_dma_underflow_armed = false;
        m_dma_underflow = false;
        m_dma_trig_timeout = false;
    }
    // Drop the instructions that are already executed.
    m_dma_insts.erase(m_dma_insts.begin(), m_dma_insts.begin() + m_dma_insts_pos);
    m_dma_insts_pos = 0;
    m_dma_insts.insert(m_dma_insts.end(), transfer.data.begin(), transfer.data.end());
    m_dma_transfers.pop_front();
    m_dma_count.fetch_add(1, std::memory_order_acq_rel);
}

NACS_INTERNAL bool DummyPulser::next_dma_group(time_point_t t, DMAGroup &group)
{
    using namespace Seq::Zynq::DMA;
    group = DMAGroup{};
    auto pos = m_dma_insts_pos;
    // Set the TTL channels `[bit, bit + width)` to `val`.
    auto set_ttl = [&] (unsigned bit, unsigned width, uint32_t val) {
        auto bank = bit / 32;
        if (bank >= NUM_TTL_BANKS)
            return;
        auto shift = bit % 32;
        auto mask = uint32_t(((uint64_t(1) << width) - 1) << shift);
        group.ttl_mask[bank] |= mask;
        group.ttl_val[bank] |= (val << shift) & mask;
    };
    auto set_dds = [&] (unsigned bus_id, unsigned dds_id, unsigned idx, uint16_t val) {
        auto chn = bus_id ? dds_id + 11 : dds_id;
        if (dds_id >= 11) {
            Log::warn("Dummy pulser: invalid DDS %d on bus %d in DMA.\n",
                      int(dds_id), int(bus_id));
            return;
        }
        group.dds.push_back({uint8_t(chn), uint8_t(idx & 0x3f), val});
    };
    while (true) {
        auto avail = m_dma_insts.size() - pos;
        size_t len = avail ? (size_t(m_dma_insts[pos] & 3) + 1) * 2 : 2;
        if (avail < len) {
            // Not enough instructions, read more if there's any.
            if (m_dma_transfers.empty() || m_dma_transfers.front().t > t)
                return false;
            pos -= m_dma_insts_pos;
            read_dma_transfer();
            continue;
        }
        auto ptr = &m_dma_insts[pos];
        pos += len;
        auto op = (ptr[0] >> 2) & 3;
        bool done = false;
        auto invalid = [&] {
            Log::warn("Dummy pulser: invalid DMA instruction (length %d, opcode %d).\n",
                      int(len), op);
        };
        switch (len) {
        case 2:
            if (op == OpCode::WAIT1) {
                group.cycle = Mem::load_unalign<Inst_v0::Wait1>(ptr).cycle;
                done = true;
            }
            else if (op == OpCode::TTL_SET4) {
                auto inst = Mem::load_unalign<Inst_v0::TTLSet4>(ptr);
                set_ttl(inst.bank4_1 * 4, 4, inst.val1);
            }
            else if (op == OpCode::CLOCKOUT) {
                group.clock_en = true;
                group.clock_period = Mem::load_unalign<Inst_v0::ClockOut>(ptr).period;
            }
            else {
                invalid();
            }
            break;
        case 4:
            if (op == OpCode::WAIT2) {
                group.cycle = Mem::load_unalign<Inst_v0::Wait2>(ptr).cycle;
                done = true;
            }
            else if (op == OpCode::TTL_SET16) {
                auto inst = Mem::load_unalign<Inst_v0::TTLSet16>(ptr);
                set_ttl(inst.bank8_1 * 8, 8, inst.val1);
                set_ttl(inst.bank8_2 * 8, 8, inst.val2);
            }
            else if (op == OpCode::DDS_SET16) {
                auto inst = Mem::load_unalign<Inst_v0::DDSSet16>(ptr);
                set_dds(inst.bus_id, inst.dds_id, inst.addr, inst.data);
            }
            else {
                invalid();
            }
            break;
        case 6:
            if (op == OpCode::WAIT_TRIG) {
                group.is_trig = true;
                group.cycle = Mem::load_unalign<Inst_v0::WaitTrig>(ptr).cycle;
                done = true;
            }
            else if (op == OpCode::TTL_SET32) {
                auto inst = Mem::load_unalign<Inst_v0::TTLSet32>(ptr);
                set_ttl(inst.bank16_1 * 16, 16, inst.val1);
                set_ttl(inst.bank16_2 * 16, 16, inst.val2);
            }
            else if (op == OpCode::DDS_SET32) {
                auto inst = Mem::load_unalign<Inst_v0::DDSSet32>(ptr);
                // The second register is always the odd one of the pair.
                set_dds(inst.bus_id, inst.dds_id, inst.addr, uint16_t(inst.data));
                set_dds(inst.bus_id, inst.dds_id, inst.addr | 1, uint16_t(inst.data >> 16));
            }
            // DAC has no effect.
            break;
        default:
            invalid();
        }
        if (done) {
            m_dma_insts_pos = pos;
            return true;
        }
    }
}

NACS_INTERNAL void DummyPulser::run_dma_group(const DMAGroup &group, time_point_t t)
{
    for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
        auto mask = group.ttl_mask[bank] & m_dma_ttl_mask[bank].load(std::memory_order_relaxed);
        if (!mask)
            continue;
        // The value outside of the mask is ignored.
        auto &ttl = m_ttl[bank];
        auto cur = ttl.load(std::memory_order_relaxed);
        while (!ttl.compare_exchange_weak(cur, (cur & ~mask) | (group.ttl_val[bank] & mask),
                                          std::memory_order_acq_rel)) {
        }
    }
    if (group.clock_en) {
        // The period in DMA cycles is `(div << 1) | 1` and 0x1ff is off.
        auto period = group.clock_period;
        m_clock.store(period >= 0x1ff ? 255 : uint8_t(period >> 1),
                      std::memory_order_release);
    }
    for (auto &dds: group.dds) {
        // The writes to the disabled registers are dropped.
        auto mask = m_dma_dds_mask[dds.chn * 32 + dds.idx / 2];
        if ((mask >> (dds.idx & 1)) & 1)
            continue;
        write_dds_word(dds.chn, dds.idx, dds.val);
    }
    // A wait takes one more cycle than its length.
    auto len = group.cycle + 1;
    if (group.is_trig) {
        // There's no trigger input.
        m_dma_trig_timeout = true;
        m_dma_cmd_end = m_dma_cmd_t;
    }
    else if (group.cycle > dma_long_wait) {
        // The commands can run until there are `dma_long_wait` cycles left.
        m_dma_cmd_t = std::max(m_dma_cmd_t, t + dma_cycle);
        m_dma_cmd_end = t + dma_cycle * (len - dma_long_wait);
    }
    else {
        m_dma_cmd_end = m_dma_cmd_t;
    }
    m_dma_fetch_t = t + dma_cycle * len;
}

NACS_INTERNAL void DummyPulser::forward_dma(time_point_t now)
{
    while (true) {
        // Run the DMA mode commands in the current long wait.
        while (!m_dma_cmds.empty()) {
            auto &cmd = m_dma_cmds.front();
            auto t = std::max(m_dma_cmd_t, cmd.t);
            if (t >= m_dma_cmd_end || t > now)
                break;
            auto steps = run_cmd(cmd);
            m_dma_cmd_t = t + std::chrono::nanoseconds(steps * 10);
            m_dma_cmds.pop();
        }
        DMAGroup group;
        if (m_dma_running) {
            if (m_dma_fetch_t > now)
                return;
            if (!next_dma_group(m_dma_fetch_t, group)) {
                // Ran out of instructions.
                m_dma_running = false;
                m_dma_underflow_armed = true;
                m_dma_cmd_end = m_dma_cmd_t;
                continue;
            }
        }
        else {
            // Start when there's a group of instructions to run.
            if (m_dma_transfers.empty())
                return;
            auto t = m_dma_transfers.front().t;
            if (t > now)
                return;
            if (!next_dma_group(t, group))
                continue;
            m_dma_running = true;
            if (m_dma_underflow_armed)
                m_dma_underflow = true;
            m_dma_fetch_t = t;
        }
        run_dma_group(group, m_dma_fetch_t);
    }
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
    case OP::WaitTrig:
        m_trig_timeout.store(true, std::memory_order_release);
        return cmd.v1;
    case OP::ClearErr:
        m_timing_ok.store(true, std::memory_order_release);
        m_trig_timeout.store(false, std::memory_order_release);
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
      m_trig_timeout(o.m_trig_timeout.load(std::memory_order_relaxed)),
      m_results(std::move(o.m_results)),
      m_cmds(std::move(o.m_cmds)),
      m_hold(o.m_hold),
      m_force_release(o.m_force_release),
      m_dds(o.m_dds),
      m_dds_cache(o.m_dds_cache),
      m_release_time(o.m_release_time),
      m_dma_dds_mask(o.m_dma_dds_mask),
      m_dma_transfers(std::move(o.m_dma_transfers)),
      m_dma_insts(std::move(o.m_dma_insts)),
      m_dma_insts_pos(o.m_dma_insts_pos),
      m_dma_running(o.m_dma_running),
      m_dma_fetch_t(o.m_dma_fetch_t),
      m_dma_underflow_armed(o.m_dma_underflow_armed),
      m_dma_underflow(o.m_dma_underflow),
      m_dma_trig_timeout(o.m_dma_trig_timeout),
      m_dma_cmds(std::move(o.m_dma_cmds)),
      m_dma_cmd_t(o.m_dma_cmd_t),
      m_dma_cmd_end(o.m_dma_cmd_end)
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
