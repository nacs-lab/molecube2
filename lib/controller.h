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

#ifndef LIBMOLECUBE_CONTROLLER_H
#define LIBMOLECUBE_CONTROLLER_H

#include "ctrl_iface.h"

#include <nacs-utils/mem.h>

#include <vector>
#include <ostream>

namespace Molecube {

class Controller : public CtrlIFace {
    Controller(const Controller&) = delete;
    void operator=(const Controller&) = delete;
    inline uint32_t read(uint32_t reg) const
    {
        return Mem::read<uint32_t>(m_addr, reg);
    }
    inline void write(uint32_t reg, uint32_t val)
    {
        Mem::write<uint32_t>(m_addr, val, reg);
    }
    struct Bits {
        enum {
            // Register 2
            TimeOK = 0x1,
            Finished = 0x4,
            NumRes = 0x1f0,
            // Register 3
            Hold = 1 << 7,
            Init = 1 << 8,
            // Control
            DDS = 0x10000000,
            Wait = 0x20000000,
            ClearErr = 0x30000000,
            LoopBack = 0x40000000,
            ClockOut = 0x50000000,
            SPI = 0x60000000,
            TimeCheck = 0x8000000,
        };
    };

    // Read
    inline uint32_t ttl_himask() const
    {
        return read(0);
    }
    inline uint32_t ttl_lomask() const
    {
        return read(1);
    }
    inline bool timing_ok() const
    {
        return !(read(2) & Bits::TimeOK);
    }
    inline bool is_finished() const
    {
        return read(2) & Bits::Finished;
    }
    inline uint32_t num_results() const
    {
        return (read(2) & Bits::NumRes) >> 4;
    }
    inline uint32_t cur_ttl() const
    {
        return read(4);
    }
    inline uint8_t cur_clock() const
    {
        return (uint8_t)read(5);
    }
    inline uint32_t pop_result() const
    {
        return read(31);
    }

    // Write
    // TTL functions: pulse_io = (ttl_out | high_mask) & (~low_mask);
    inline void set_ttl_himask(uint32_t high_mask)
    {
        write(0, high_mask);
    }
    inline void set_ttl_lomask(uint32_t low_mask)
    {
        write(1, low_mask);
    }
    // release hold.  pulses can run
    inline void release_hold()
    {
        write(3, read(3) & ~Bits::Hold);
    }
    // set hold. pulses are stopped
    inline void set_hold()
    {
        write(3, read(3) | Bits::Hold);
    }
    // toggle init. reset prior to new sequence
    inline void toggle_init()
    {
        uint32_t r3 = read(3);
        write(3, r3 | Bits::Init);
        write(3, r3 & ~Bits::Init);
    }

    // Pulses
    template<bool checked>
    inline void short_pulse(uint32_t ctrl, uint32_t op)
    {
        if (checked)
            ctrl = ctrl | Bits::TimeCheck;
        write(31, op);
        write(31, ctrl);
    }
    template<bool checked>
    inline void ttl_pulse(uint32_t ttl, uint32_t t)
    {
        assert(t < (1 << 24));
        short_pulse<checked>(t, ttl);
    }
    template<bool checked>
    inline void clock_pulse(uint8_t div)
    {
        short_pulse<checked>(Bits::ClockOut, div & 0xff);
    }
    template<bool checked>
    inline void spi_pulse(uint8_t clk_div, uint8_t spi_id, uint32_t data)
    {
        uint32_t opcode = ((uint32_t(spi_id & 3) << 11) | clk_div);
        short_pulse<checked>(opcode | Bits::SPI, data);
    }
    template<bool checked>
    inline void dac_pulse(uint8_t dac, uint16_t V)
    {
        spi_pulse<checked>(0, 0, ((dac & 3) << 16) | V);
    }
    template<bool checked>
    inline void dds_pulse(uint32_t ctrl, uint32_t op)
    {
        short_pulse<checked>(Bits::DDS | ctrl, op);
    }
    // set bytes at addr + 1 and addr
    template<bool checked>
    inline void dds_set_2bytes_pulse(int i, uint32_t addr, uint32_t data)
    {
        // put addr in bits 15...9 (maps to DDS opcode_reg[14:9])?
        // put data in bits 15...0 (maps to DDS operand_reg[15:0])?
        dds_pulse<checked>(0x2 | (i << 4) | (((addr + 1) & 0x7f) << 9), data & 0xffff);
    }
    // set bytes addr + 3 ... addr
    template<bool checked>
    inline void dds_set_4bytes_pulse(int i, uint32_t addr, uint32_t data)
    {
        // put addr in bits 15...9 (maps to DDS opcode_reg[14:9])?
        dds_pulse<checked>(0xf | (i << 4) | (((addr + 1) & 0x7f) << 9), data);
    }
    template<bool checked>
    inline void wait_pulse(uint32_t t)
    {
        assert(t < (1 << 24));
        short_pulse<checked>(Bits::Wait | t, 0);
    }
    // clear timing check (clear failures)
    template<bool checked>
    inline void clear_error_pulse()
    {
        short_pulse<checked>(Bits::ClearErr, 0);
    }
    template<bool checked>
    inline void dds_set_freq_pulse(int i, uint32_t ftw)
    {
        return dds_pulse<checked>(i << 4, ftw);
    }
    template<bool checked>
    inline void dds_set_amp_pulse(int i, uint16_t amp)
    {
        return dds_set_2bytes_pulse<checked>(i, 0x32, amp);
    }
    template<bool checked>
    inline void dds_set_phase_pulse(int i, uint16_t phase)
    {
        return dds_set_2bytes_pulse<checked>(i, 0x30, phase);
    }
    template<bool checked>
    inline void dds_reset_pulse(int i)
    {
        return dds_pulse<checked>(0x4 | (i << 4), 0);
    }

    // Pulses with results
    // clear timing check (clear failures)
    template<bool checked>
    inline void loopback_pulse(uint32_t data)
    {
        short_pulse<checked>(Bits::LoopBack, data);
    }
    template<bool checked>
    inline void dds_get_2bytes_pulse(int i, uint32_t addr)
    {
        dds_pulse<checked>(0x3 | (i << 4) | ((addr + 1) << 9), 0);
    }
    template<bool checked>
    inline void dds_get_4bytes_pulse(int i, uint32_t addr)
    {
        dds_pulse<checked>(0xe | (i << 4) | ((addr + 1) << 9), 0);
    }
    template<bool checked>
    inline void dds_get_phase_pulse(int i)
    {
        dds_get_2bytes_pulse<checked>(i, 0x30);
    }
    template<bool checked>
    inline void dds_get_amp_pulse(int i)
    {
        dds_get_2bytes_pulse<checked>(i, 0x32);
    }
    template<bool checked>
    inline void dds_get_freq_pulse(int i)
    {
        dds_get_4bytes_pulse<checked>(i, 0x2c);
    }
public:
    Controller();

private:
    bool concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t val) override;
    bool concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t &val) override;
    bool try_get_result(uint32_t &res);
    uint32_t get_result();

    void init_dds(int chn);
    bool check_dds(int chn);
    bool dds_exists(int chn);
    void dump_dds(std::ostream &stm, int chn);

    static constexpr uint8_t NDDS = 22;
    static constexpr uint32_t magic_bytes = 0xf00f0000;

    volatile void *const m_addr;
    DDSState m_dds_ovr[NDDS];
    uint16_t m_dds_phase[NDDS] = {0};
    // Reinitialize is a complicated sequence and is rarely needed
    // so only do that after the sequence finishes.
    bool m_dds_pending_reset[NDDS] = {false};
};

}

#endif
