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
    inline uint32_t ttl_himask() const
    {
        return m_ttl_hi.load(std::memory_order_acquire);
    }
    inline uint32_t ttl_lomask() const
    {
        return m_ttl_lo.load(std::memory_order_acquire);
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
    inline uint32_t cur_ttl() const
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_ttl.load(std::memory_order_acquire);
    }
    inline uint8_t cur_clock() const
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_clock.load(std::memory_order_acquire);
    }

    // Write
    inline void set_ttl_himask(uint32_t high_mask)
    {
        m_ttl_hi.store(high_mask, std::memory_order_release);
    }
    inline void set_ttl_lomask(uint32_t low_mask)
    {
        m_ttl_lo.store(low_mask, std::memory_order_release);
    }
    void release_hold();
    void set_hold();
    void toggle_init();

    // Pulses
    template<bool checked>
    inline void ttl(uint32_t ttl, uint32_t t)
    {
        assert(t <= max_wait_t);
        add_cmd(OP::TTL, checked, t, ttl);
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

    // Debug registers
    inline uint32_t loopback_reg()
    {
        return m_loopback_reg.load(std::memory_order_relaxed);
    }
    inline void set_loopback_reg(uint32_t val)
    {
        m_loopback_reg.store(val, std::memory_order_relaxed);
    }
    inline uint32_t inst_word_count()
    {
        return m_inst_word_count.load(std::memory_order_relaxed);
    }
    inline uint32_t inst_count()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_inst_count.load(std::memory_order_relaxed);
    }
    inline uint32_t ttl_count()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_ttl_count.load(std::memory_order_relaxed);
    }
    inline uint32_t dds_count()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_dds_count.load(std::memory_order_relaxed);
    }
    inline uint32_t wait_count()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_wait_count.load(std::memory_order_relaxed);
    }
    inline uint32_t clear_error_count()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_clear_error_count.load(std::memory_order_relaxed);
    }
    inline uint32_t loopback_count()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_loopback_count.load(std::memory_order_relaxed);
    }
    inline uint32_t clock_count()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_clock_count.load(std::memory_order_relaxed);
    }
    inline uint32_t spi_count()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_spi_count.load(std::memory_order_relaxed);
    }
    inline uint32_t underflow_cycle()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_underflow_cycle.load(std::memory_order_relaxed);
    }
    inline uint32_t inst_cycle()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_inst_cycle.load(std::memory_order_relaxed);
    }
    inline uint32_t ttl_cycle()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_ttl_cycle.load(std::memory_order_relaxed);
    }
    inline uint32_t wait_cycle()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_wait_cycle.load(std::memory_order_relaxed);
    }
    inline uint32_t result_overflow_count()
    {
        auto rc = result_count();
        if (rc > max_result_count)
            return rc - max_result_count;
        return 0;
    }
    inline uint32_t result_count()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return uint32_t(m_results.size());
    }
    inline uint32_t result_generated()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_result_generated.load(std::memory_order_relaxed);
    }
    inline uint32_t result_consumed()
    {
        const_cast<DummyPulser*>(this)->forward_time();
        return m_result_consumed.load(std::memory_order_relaxed);
    }

    DummyPulser();
    DummyPulser(DummyPulser &&other);

    bool try_get_result(uint32_t &res);
    uint32_t get_result();

    void init_dds(int chn);
    bool check_dds(int chn, bool force);
    bool dds_exists(int chn);
    void dump_dds(std::ostream &stm, int chn);

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

    std::atomic<uint32_t> m_ttl_hi{0};
    std::atomic<uint32_t> m_ttl_lo{0};
    std::atomic<uint32_t> m_ttl{0};
    std::atomic<uint8_t> m_clock{255};
    std::atomic<bool> m_cmds_empty{true};
    std::atomic<bool> m_timing_ok{true};
    std::atomic<bool> m_timing_check{false};

    // Debug registers
    std::atomic<uint32_t> m_loopback_reg{0};
    std::atomic<uint32_t> m_inst_word_count{0};
    std::atomic<uint32_t> m_inst_count{0};
    std::atomic<uint32_t> m_ttl_count{0};
    std::atomic<uint32_t> m_dds_count{0};
    std::atomic<uint32_t> m_wait_count{0};
    std::atomic<uint32_t> m_clear_error_count{0};
    std::atomic<uint32_t> m_loopback_count{0};
    std::atomic<uint32_t> m_clock_count{0};
    std::atomic<uint32_t> m_spi_count{0};
    std::atomic<uint32_t> m_underflow_cycle{0};
    std::atomic<uint32_t> m_inst_cycle{0};
    std::atomic<uint32_t> m_ttl_cycle{0};
    std::atomic<uint32_t> m_wait_cycle{0};
    std::atomic<uint32_t> m_result_generated{0};
    std::atomic<uint32_t> m_result_consumed{0};

    std::mutex m_cmds_lock;

    // This isn't a very efficient implementation of fifo but we don't really care.
    // It has the same semantic as the hardware one and that's more important.
    std::queue<uint32_t> m_results;
    std::queue<Cmd> m_cmds;
    bool m_hold{false};
    bool m_force_release{false};

    std::array<DDS,NDDS> m_dds;

    time_point_t m_release_time{std::chrono::steady_clock::now()};
};

}

#endif
