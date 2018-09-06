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

#include <atomic>
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
        uint32_t v1;
        uint32_t v2;
    };
public:
    // Read
    inline uint32_t ttl_himask() const
    {
        return m_ttl_hi.load(std::memory_order_acquire);
    }
    inline uint32_t ttl_lomask() const
    {
        return m_ttl_lo.load(std::memory_order_acquire);
    }
    inline bool is_finished() const
    {
        return m_cmds_empty.load(std::memory_order_acquire);
    }
    inline uint32_t cur_ttl() const
    {
        if (!m_cmds_empty.load(std::memory_order_acquire)) {
            // TODO
        }
        return m_ttl.load(std::memory_order_acquire);
    }
    inline uint8_t cur_clock() const
    {
        if (!m_cmds_empty.load(std::memory_order_acquire)) {
            // TODO
        }
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

    // Pulses
    template<bool checked>
    inline void ttl(uint32_t ttl, uint32_t t)
    {
        assert(t < (1 << 24));
        add_cmd(Cmd{OP::TTL, checked, t, ttl});
    }
    template<bool checked>
    inline void clock(uint8_t div)
    {
        add_cmd(Cmd{OP::Clock, checked, div, 0});
    }
    template<bool checked>
    inline void dac(uint8_t dac, uint16_t V)
    {
        add_cmd(Cmd{OP::DAC, checked, dac, V});
    }
    template<bool checked>
    inline void wait(uint32_t t)
    {
        assert(t < (1 << 24));
        add_cmd(Cmd{OP::Wait, checked, t, 0});
    }
    // clear timing check (clear failures)
    inline void clear_error()
    {
        add_cmd(Cmd{OP::ClearErr, false, 0, 0});
    }
    template<bool checked>
    inline void dds_set_freq(int i, uint32_t ftw)
    {
        add_cmd(Cmd{OP::ClearErr, false, 0, 0});
    }
    template<bool checked>
    inline void dds_set_amp(int i, uint16_t amp)
    {
        add_cmd(Cmd{OP::DDSSetAmp, checked, i, amp});
    }
    template<bool checked>
    inline void dds_set_phase(int i, uint16_t phase)
    {
        add_cmd(Cmd{OP::DDSSetPhase, checked, i, phase});
    }
    template<bool checked>
    inline void dds_reset(int i)
    {
        add_cmd(Cmd{OP::DDSReset, checked, i, 0});
    }

    // Pulses with results
    // clear timing check (clear failures)
    template<bool checked>
    inline void loopback(uint32_t data)
    {
        add_cmd(Cmd{OP::LoopBack, checked, data, 0});
    }
    template<bool checked>
    inline void dds_get_phase(int i)
    {
        add_cmd(Cmd{OP::DDSGetPhase, checked, i, 0});
    }
    template<bool checked>
    inline void dds_get_amp(int i)
    {
        add_cmd(Cmd{OP::DDSGetAmp, checked, i, 0});
    }
    template<bool checked>
    inline void dds_get_freq(int i)
    {
        add_cmd(Cmd{OP::DDSGetFreq, checked, i, 0});
    }
    DummyPulser();

    bool try_get_result(uint32_t &res);
    uint32_t get_result();

    void init_dds(int chn);
    bool check_dds(int chn, bool force);
    bool dds_exists(int chn);
    void dump_dds(std::ostream &stm, int chn);

private:
    void add_result(uint32_t v);
    void add_cmd(Cmd cmd);

    static constexpr int NDDS = 22;

    std::atomic<uint32_t> m_ttl_hi{0};
    std::atomic<uint32_t> m_ttl_lo{0};
    std::atomic<uint32_t> m_ttl{0};
    std::atomic<uint8_t> m_clock{255};

    // This isn't a very efficient implementation of fifo but we don't really care.
    // It has the same semantic as the hardware one and that's more important.
    std::queue<uint32_t> m_results;
    std::queue<Cmd> m_cmds;
    std::atomic<bool> m_cmds_empty{true};

    DDS m_dds[NDDS];
};

}

#endif
