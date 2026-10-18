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

#ifndef LIBMOLECUBE_DUMMY_PULSER_H
#define LIBMOLECUBE_DUMMY_PULSER_H

#include "pulser_common.h"

#include <nacs-utils/utils.h>

#include <assert.h>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <ostream>
#include <queue>
#include <vector>

namespace Molecube {

using namespace NaCs;

/**
 * This is a dummy implementation of `Pulser`
 * that provide the same API and can be used for testing.
 *
 * To simplify the implementation, functions that require access to the command or result
 * fifo's (include all DDS functions) are assumed to be called only from a single thread.
 * Hold/release/init/timing functions should also only be called from this thread.
 * Other functions (current ttl, clock, DMA) can be called from any threads.
 *
 * The DMA control bit selects the mode of the command queue.
 * In the DMA mode, only the DDS and DAC commands are accepted (others are dropped)
 * and they are only run during the long waits of the DMA sequences, as the hardware.
 * The DMA sequences are executed (in real time) only in the DMA mode.
 */
class DummyPulser {
    using time_point_t = decltype(std::chrono::steady_clock::now());
    DummyPulser(const DummyPulser&) = delete;
    void operator=(const DummyPulser&) = delete;
    // The 16 bit registers of a DDS (byte address 0 to 0x7f).
    // All registers are treated as normal memory (read returns the last value written).
    using DDSRegs = std::array<uint16_t,64>;
    enum class OP : uint8_t {
        TTL,
        Clock,
        DAC,
        Wait,
        ClearErr,
        // For all the DDS register read/write commands,
        // `v1` is the DDS channel number with the byte address in bits 8 and up.
        DDSSet2,
        DDSSet4,
        DDSReset,
        LoopBack,
        DDSGet2,
        DDSGet4,
    };
    struct Cmd {
        OP op;
        bool timing;
        time_point_t t;
        uint32_t v1;
        uint32_t v2;
    };
public:
    static constexpr uint32_t max_wait_t = (1 << 24) - 1;
    // Read
    inline uint32_t ttl_himask(int bank) const
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        return m_ttl_hi[bank].load(std::memory_order_acquire);
    }
    inline uint32_t ttl_lomask(int bank) const
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        return m_ttl_lo[bank].load(std::memory_order_acquire);
    }
    inline bool timing_ok() const
    {
        if (!m_timing_ok.load(std::memory_order_acquire))
            return false;
        const_cast<DummyPulser*>(this)->forward_time();
        return m_timing_ok.load(std::memory_order_acquire);
    }
    inline bool is_finished() const
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_cmds_empty.load(std::memory_order_acquire);
    }
    inline uint32_t cur_ttl(int bank) const
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        const_cast<DummyPulser*>(this)->forward_all();
        return m_ttl[bank].load(std::memory_order_acquire);
    }
    inline uint8_t cur_clock() const
    {
        const_cast<DummyPulser*>(this)->forward_all();
        return m_clock.load(std::memory_order_acquire);
    }
    inline uint32_t ttl_in(int bank) const
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        (void)bank;
        return 0;
    }
    inline uint32_t dma_ttl_mask(int bank) const
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        return m_dma_ttl_mask[bank].load(std::memory_order_acquire);
    }
    // Same bits as `DMAStatus` in the hardware.
    uint32_t dma_status() const;
    inline uint32_t dma_control() const
    {
        return m_dma_control.load(std::memory_order_acquire);
    }

    // Write
    inline void set_ttl_himask(uint32_t high_mask, int bank)
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        m_ttl_hi[bank].store(high_mask, std::memory_order_release);
    }
    inline void set_ttl_lomask(uint32_t low_mask, int bank)
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        m_ttl_lo[bank].store(low_mask, std::memory_order_release);
    }
    inline void set_clock(uint8_t div)
    {
        m_clock.store(div, std::memory_order_release);
    }
    // Set the channels in `hi` to high and the ones in `lo` to low
    // in the byte `bytes` of the TTL output.
    inline void set_ttl(int bytes, uint8_t lo, uint8_t hi)
    {
        assert(bytes >= 0 && bytes < NUM_TTL_BANKS * 4);
        auto &ttl = m_ttl[bytes / 4];
        auto shift = (bytes % 4) * 8;
        auto cur = ttl.load(std::memory_order_relaxed);
        uint32_t new_ttl;
        do {
            new_ttl = (cur | (uint32_t(hi) << shift)) & ~(uint32_t(lo) << shift);
        } while (!ttl.compare_exchange_weak(cur, new_ttl, std::memory_order_acq_rel));
    }
    inline void set_dma_ttl_mask(int bank, uint32_t mask)
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        m_dma_ttl_mask[bank].store(mask, std::memory_order_release);
    }
    // Start a DMA transfer of `blocks + 1` blocks (128 bytes each)
    // of instructions at `addr` (from `buffer_addr`).
    // The instructions are copied when this is called.
    // Ignored (with a warning) if the DMA mode is not enabled.
    void start_dma(uintptr_t addr, uint16_t blocks, bool first);
    // Only the lowest bit (DMA mode enable) is used.
    void set_dma_control(uint32_t ctrl);
    // Read the cached value of the 16 bit DDS register at the byte address `dds_addr`
    // of DDS `dds_id` on bus 0/1.
    // The cache is updated with the register value on each DDS register read/write
    // command but not on a DDS reset, the same as the hardware.
    inline uint16_t read_dds0(uint8_t dds_id, uint8_t dds_addr)
    {
        return read_dds(dds_id, dds_addr);
    }
    inline uint16_t read_dds1(uint8_t dds_id, uint8_t dds_addr)
    {
        return read_dds(dds_id + 11, dds_addr);
    }
    inline void set_dma_dds_mask(int chn, uint8_t addr, uint8_t mask)
    {
        assert(chn >= 0 && chn < NDDS && addr < 0x80);
        std::lock_guard<std::mutex> lock(m_cmds_lock);
        m_dma_dds_mask[chn * 32 + addr / 4] = uint8_t(mask & 3);
    }
    inline uint8_t get_dma_dds_mask(int chn, uint8_t addr) const
    {
        assert(chn >= 0 && chn < NDDS && addr < 0x80);
        std::lock_guard<std::mutex> lock(m_cmds_lock);
        return m_dma_dds_mask[chn * 32 + addr / 4];
    }
    void release_hold();
    void set_hold();
    void toggle_init();

    // Pulses
    template<bool checked>
    inline void ttl(uint32_t ttl, uint32_t t, int bank)
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        assert(t <= max_wait_t);
        add_cmd(OP::TTL, checked, t | (uint32_t(bank) << 24), ttl);
    }
    template<bool checked>
    inline void clock(uint8_t div)
    {
        add_cmd(OP::Clock, checked, div);
    }
    template<bool checked>
    inline void dac(uint8_t dac, uint16_t V)
    {
        add_cmd(OP::DAC, checked, dac, V);
    }
    template<bool checked>
    inline void wait(uint32_t t)
    {
        assert(t <= max_wait_t);
        add_cmd(OP::Wait, checked, t);
    }
    // clear timing check (clear failures)
    inline void clear_error()
    {
        add_cmd(OP::ClearErr, false);
    }
    // set bytes addr + 1 ... addr
    template<bool checked>
    inline void dds_set_2bytes(int i, uint32_t addr, uint32_t data)
    {
        add_cmd(OP::DDSSet2, checked, dds_reg_arg(i, addr), data & 0xffff);
    }
    // set bytes addr + 3 ... addr
    template<bool checked>
    inline void dds_set_4bytes(int i, uint32_t addr, uint32_t data)
    {
        add_cmd(OP::DDSSet4, checked, dds_reg_arg(i, addr), data);
    }
    template<bool checked>
    inline void dds_get_2bytes(int i, uint32_t addr)
    {
        add_cmd(OP::DDSGet2, checked, dds_reg_arg(i, addr));
    }
    template<bool checked>
    inline void dds_get_4bytes(int i, uint32_t addr)
    {
        add_cmd(OP::DDSGet4, checked, dds_reg_arg(i, addr));
    }
    template<bool checked>
    inline void dds_set_freq(int i, uint32_t ftw)
    {
        dds_set_4bytes<checked>(i, 0x2c, ftw);
    }
    template<bool checked>
    inline void dds_set_amp(int i, uint16_t amp)
    {
        dds_set_2bytes<checked>(i, 0x32, amp);
    }
    template<bool checked>
    inline void dds_set_phase(int i, uint16_t phase)
    {
        dds_set_2bytes<checked>(i, 0x30, phase);
    }
    template<bool checked>
    inline void dds_reset(int i)
    {
        assert(i < NDDS);
        add_cmd(OP::DDSReset, checked, i);
    }

    // Pulses with results
    // clear timing check (clear failures)
    template<bool checked>
    inline void loopback(uint32_t data)
    {
        add_cmd(OP::LoopBack, checked, data);
    }
    template<bool checked>
    inline void dds_get_phase(int i)
    {
        dds_get_2bytes<checked>(i, 0x30);
    }
    template<bool checked>
    inline void dds_get_amp(int i)
    {
        dds_get_2bytes<checked>(i, 0x32);
    }
    template<bool checked>
    inline void dds_get_freq(int i)
    {
        dds_get_4bytes<checked>(i, 0x2c);
    }
    template<bool checked>
    inline void wait_trigger(uint8_t, bool, uint32_t timeout)
    {
        wait<checked>(timeout);
    }

    // Debug registers
    inline uint32_t loopback_reg()
    {
        return m_loopback_reg.load(std::memory_order_relaxed);
    }
    inline void set_loopback_reg(uint32_t val)
    {
        m_loopback_reg.store(val, std::memory_order_relaxed);
    }

    pulser_version_t hw_version() const
    {
        return {5, 4};
    }

    void set_dds_timing1(uint32_t adsu, uint32_t wrlow, uint32_t adhd,
                         uint32_t fuddl, uint32_t fudhd)
    {
        m_dds_adsu = adsu & 7;
        m_dds_wrlow = wrlow & 7;
        m_dds_adhd = adhd & 7;
        m_dds_fuddl = fuddl & 7;
        m_dds_fudhd = fudhd & 7;
    }
    std::array<uint8_t,5> get_dds_timing1() const
    {
        return {m_dds_adsu, m_dds_wrlow, m_dds_adhd,
                m_dds_fuddl, m_dds_fudhd};
    }

    DummyPulser();
    DummyPulser(DummyPulser &&other);

    bool try_get_result(uint32_t &res);
    uint32_t get_result();

    // `dma` is ignored since the commands are run without the DMA.
    void init_dds(int chn, bool dma=false);
    bool check_dds(int chn, bool force, bool dma=false);
    bool dds_exists(int chn);
    void dump_dds(std::ostream &stm, int chn);

    static void *alloc_buffer(size_t size);
    static uintptr_t buffer_addr(void *buff);
    static void free_buffer(void *buff, size_t size);

    bool support_dma() const
    {
        return true;
    }

private:
    // check dds existance without changing debug registers.
    bool dds_exists_internal(int chn)
    {
        return 0 <= chn && chn < NDDS;
    }
    // Encode the DDS channel and the byte address for the DDS register commands.
    static uint32_t dds_reg_arg(int chn, uint32_t addr)
    {
        assert(0 <= chn && chn < NDDS);
        // The 16 bit registers are aligned.
        assert(addr % 2 == 0);
        return uint32_t(chn) | ((addr & 0x7f) << 8);
    }

    uint16_t read_dds(int chn, uint8_t addr);
    // Push a result to the result queue. Check if there's overflow
    void add_result(uint32_t v);
    // Add a command to the command queue.
    // If the command queue is full, start executing and wait until it's not full anymore.
    void add_cmd(OP op, bool timing, uint32_t v1=0, uint32_t v2=0);
    // Handle overdue commands in the command queue.
    // If `block` is `true`, wait until at least one command is executed.
    // Throw an error if `block` is `true` and the command queue is empty.
    void forward_time(bool block=false)
    {
        if (m_cmds_empty.load(std::memory_order_acquire))
            return;
        std::unique_lock<std::mutex> lock(m_cmds_lock);
        forward_time(block, lock);
    }
    void forward_time(bool block, std::unique_lock<std::mutex> &lock);
    // Run the command (apply the side-effects) and return the time
    // it takes to execute the command in FPGA time step (10ns per step).
    uint32_t run_cmd(const Cmd &cmd);
    // Write/read the 16 bit DDS register for `arg` (see `dds_reg_arg`)
    // and update the cache in the hardware.
    void write_dds_reg(uint32_t arg, uint16_t val)
    {
        write_dds_word(arg & 0xff, ((arg >> 8) & 0x7f) >> 1, val);
    }
    void write_dds_word(unsigned chn, unsigned idx, uint16_t val)
    {
        m_dds[chn][idx] = val;
        m_dds_cache[chn][idx] = val;
    }
    uint16_t read_dds_reg(uint32_t arg)
    {
        auto chn = arg & 0xff;
        auto idx = ((arg >> 8) & 0x7f) >> 1;
        auto val = m_dds[chn][idx];
        m_dds_cache[chn][idx] = val;
        return val;
    }
    // Run the commands that should be executed before the specified time.
    // Return if any command is run
    bool run_past_cmds(time_point_t t);

    // Handle everything (the commands and the DMA sequence) that should have
    // happened by now.
    void forward_all()
    {
        std::unique_lock<std::mutex> lock(m_cmds_lock);
        forward_all(lock);
    }
    void forward_all(std::unique_lock<std::mutex> &lock);

    // DMA
    // A DMA transfer that hasn't been read by the DMA yet.
    struct DMATransfer {
        // Time when the transfer is started.
        time_point_t t;
        bool first;
        std::vector<uint8_t> data;
    };
    // The output actions of the DMA instructions before a wait and the wait.
    struct DMAGroup {
        // TTL values and the channels set.
        std::array<uint32_t,NUM_TTL_BANKS> ttl_val{};
        std::array<uint32_t,NUM_TTL_BANKS> ttl_mask{};
        bool clock_en{false};
        uint16_t clock_period{0};
        // DDS channel, 16 bit register index and value
        struct DDSWrite {
            uint8_t chn;
            uint8_t idx;
            uint16_t val;
        };
        std::vector<DDSWrite> dds;
        bool is_trig{false};
        uint64_t cycle{0};
    };
    // Run the DMA sequence (and the commands in the DMA mode) until `t`.
    void forward_dma(time_point_t t);
    // Parse the next group of DMA instructions with the transfers started by `t`.
    // Returns false if there are not enough instructions for a group.
    bool next_dma_group(time_point_t t, DMAGroup &group);
    // Read the next transfer into `m_dma_insts`.
    void read_dma_transfer();
    // Apply the output actions of `group` at `t` and start the wait.
    void run_dma_group(const DMAGroup &group, time_point_t t);

    static constexpr int NDDS = 22;
    static constexpr uint32_t max_result_count = 4097;
    // The DMA runs at 200MHz.
    static constexpr std::chrono::nanoseconds dma_cycle{5};
    // The commands only run in a DMA wait while there are more than this many cycles left.
    static constexpr uint64_t dma_long_wait = 128;
    // Number of commands in the DMA mode queue to report it as full.
    // (The hardware has a 32 entry queue for each of the DDS buses and some buffer before it)
    static constexpr size_t dma_cmd_queue_size = 32;
    // Same as `Pulser`.
    static constexpr uint32_t magic_bytes = 0xf00f0000;

    std::array<std::atomic<uint32_t>,NUM_TTL_BANKS> m_ttl_hi{0};
    std::array<std::atomic<uint32_t>,NUM_TTL_BANKS> m_ttl_lo{0};
    std::array<std::atomic<uint32_t>,NUM_TTL_BANKS> m_ttl{0};
    std::array<std::atomic<uint32_t>,NUM_TTL_BANKS> m_dma_ttl_mask{0};
    std::atomic<uint32_t> m_dma_count{0};
    std::atomic<uint32_t> m_dma_control{0};
    std::atomic<uint8_t> m_clock{255};
    std::atomic<bool> m_cmds_empty{true};
    std::atomic<bool> m_timing_ok{true};
    std::atomic<bool> m_timing_check{false};

    // Debug registers
    std::atomic<uint32_t> m_loopback_reg{0};

    mutable std::mutex m_cmds_lock;

    // This isn't a very efficient implementation of fifo but we don't really care.
    // It has the same semantic as the hardware one and that's more important.
    std::queue<uint32_t> m_results;
    std::queue<Cmd> m_cmds;
    bool m_hold{false};
    bool m_force_release{false};

    // The registers of the DDS's.
    std::array<DDSRegs,NDDS> m_dds{};
    // The cache of the DDS registers in the hardware read by `read_dds0/1`.
    std::array<DDSRegs,NDDS> m_dds_cache{};

    time_point_t m_release_time{std::chrono::steady_clock::now()};

    uint8_t m_dds_adsu{7};
    uint8_t m_dds_wrlow{7};
    uint8_t m_dds_adhd{7};
    uint8_t m_dds_fuddl{7};
    uint8_t m_dds_fudhd{7};

    // DMA DDS write disable bits for each pair of 16 bit registers.
    std::array<uint8_t,NDDS * 32> m_dma_dds_mask{};

    // The DMA state. All of these are protected by `m_cmds_lock`.
    // The transfers started but not read yet.
    std::deque<DMATransfer> m_dma_transfers;
    // The instructions read but not executed yet (starting from `m_dma_insts_pos`).
    std::vector<uint8_t> m_dma_insts;
    size_t m_dma_insts_pos{0};
    // Whether the DMA is running the instructions, and when the next group starts.
    bool m_dma_running{false};
    time_point_t m_dma_fetch_t{};
    // The DMA status flags.
    bool m_dma_underflow_armed{false};
    bool m_dma_underflow{false};
    bool m_dma_trig_timeout{false};
    // The commands in the DMA mode, run in the long DMA waits.
    std::queue<Cmd> m_dma_cmds;
    // Time when the next DMA mode command may start and the end of the current long wait.
    time_point_t m_dma_cmd_t{};
    time_point_t m_dma_cmd_end{};
};

}

#endif
