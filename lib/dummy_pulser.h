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
#include <mutex>
#include <ostream>
#include <queue>

namespace Molecube {

using namespace NaCs;

/**
 * This is a dummy implementation of `Pulser`
 * that provide the same API and can be used for testing.
 *
 * To simplify the implementation, functions that require access to the command or result
 * fifo's (include all DDS functions) are assumed to be called only from a single thread.
 * Hold/release/init/timing functions should also only be called from this thread.
 * Other functions (current ttl, clock) can be called from any threads.
 */
class DummyPulser {
    using time_point_t = decltype(std::chrono::steady_clock::now());
    DummyPulser(const DummyPulser&) = delete;
    void operator=(const DummyPulser&) = delete;
    struct DDS {
        bool init{false};
        uint16_t amp{0};
        uint16_t phase{0};
        uint32_t freq{0};
    };
    enum class OP : uint8_t {
        TTL,
        Clock,
        DAC,
        Wait,
        ClearErr,
        DDSSetFreq,
        DDSSetAmp,
        DDSSetPhase,
        DDSReset,
        LoopBack,
        DDSGetFreq,
        DDSGetAmp,
        DDSGetPhase,
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
        const_cast<DummyPulser*>(this)->forward_time();
        return m_ttl[bank].load(std::memory_order_acquire);
    }
    inline uint8_t cur_clock() const
    {
        const_cast<DummyPulser*>(this)->forward_time();
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
    inline uint32_t dma_status() const
    {
        // The DMA is never busy since the instructions aren't executed.
        return m_dma_count.load(std::memory_order_acquire) & 0xff;
    }
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
    inline void set_dma_ttl_mask(int bank, uint32_t mask)
    {
        assert(bank >= 0 && bank < NUM_TTL_BANKS);
        m_dma_ttl_mask[bank].store(mask, std::memory_order_release);
    }
    // Only keeps track of the number of DMA requests.
    // The instructions are not executed.
    inline void start_dma(uintptr_t, uint16_t, bool)
    {
        m_dma_count.fetch_add(1, std::memory_order_acq_rel);
    }
    // Only stores the value. It has no effect otherwise.
    // Only the lowest bit is used.
    inline void set_dma_control(uint32_t ctrl)
    {
        m_dma_control.store(ctrl & 1, std::memory_order_release);
    }
    // Only stores the value since the DMA instructions are not executed.
    inline void set_dma_dds_mask(int chn, uint8_t addr, uint8_t mask)
    {
        assert(chn >= 0 && chn < NDDS && addr < 0x80);
        m_dma_dds_mask[chn * 32 + addr / 4] = uint8_t(mask & 3);
    }
    inline uint8_t get_dma_dds_mask(int chn, uint8_t addr) const
    {
        assert(chn >= 0 && chn < NDDS && addr < 0x80);
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
    template<bool checked>
    inline void dds_set_freq(int i, uint32_t ftw)
    {
        assert(i < NDDS);
        add_cmd(OP::DDSSetFreq, checked, i, ftw);
    }
    template<bool checked>
    inline void dds_set_amp(int i, uint16_t amp)
    {
        assert(i < NDDS);
        add_cmd(OP::DDSSetAmp, checked, i, amp);
    }
    template<bool checked>
    inline void dds_set_phase(int i, uint16_t phase)
    {
        assert(i < NDDS);
        add_cmd(OP::DDSSetPhase, checked, i, phase);
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
        assert(i < NDDS);
        add_cmd(OP::DDSGetPhase, checked, i);
    }
    template<bool checked>
    inline void dds_get_amp(int i)
    {
        assert(i < NDDS);
        add_cmd(OP::DDSGetAmp, checked, i);
    }
    template<bool checked>
    inline void dds_get_freq(int i)
    {
        assert(i < NDDS);
        add_cmd(OP::DDSGetFreq, checked, i);
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

    void init_dds(int chn);
    bool check_dds(int chn, bool force);
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
    // Run the commands that should be executed before the specified time.
    // Return if any command is run
    bool run_past_cmds(time_point_t t);

    static constexpr int NDDS = 22;
    static constexpr uint32_t max_result_count = 4097;

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

    std::mutex m_cmds_lock;

    // This isn't a very efficient implementation of fifo but we don't really care.
    // It has the same semantic as the hardware one and that's more important.
    std::queue<uint32_t> m_results;
    std::queue<Cmd> m_cmds;
    bool m_hold{false};
    bool m_force_release{false};

    std::array<DDS,NDDS> m_dds;

    time_point_t m_release_time{std::chrono::steady_clock::now()};

    uint8_t m_dds_adsu{7};
    uint8_t m_dds_wrlow{7};
    uint8_t m_dds_adhd{7};
    uint8_t m_dds_fuddl{7};
    uint8_t m_dds_fudhd{7};

    // DMA DDS write disable bits for each pair of 16 bit registers.
    std::array<uint8_t,NDDS * 32> m_dma_dds_mask{};
};

}

#endif
