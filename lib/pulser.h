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

#ifndef LIBMOLECUBE_PULSER_H
#define LIBMOLECUBE_PULSER_H

#include <nacs-utils/mem.h>

#include <assert.h>

#include <ostream>

namespace Molecube {

using namespace NaCs;

/**
 * This class contains the stateless functions to communicate with the FPGA
 */
class Pulser {
    Pulser(const Pulser&) = delete;
    void operator=(const Pulser&) = delete;
    // Read and write pulse controller registers.
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
    inline uint32_t pop_result() const
    {
        return read(31);
    }
    inline uint32_t num_results() const
    {
        return (read(2) & Bits::NumRes) >> 4;
    }

    // Internal pulses
    template<bool checked>
    inline void pulse(uint32_t ctrl, uint32_t op)
    {
        if (checked)
            ctrl = ctrl | Bits::TimeCheck;
        write(31, op);
        write(31, ctrl);
    }
    template<bool checked>
    inline void spi(uint8_t clk_div, uint8_t spi_id, uint32_t data)
    {
        uint32_t opcode = ((uint32_t(spi_id & 3) << 11) | clk_div);
        pulse<checked>(opcode | Bits::SPI, data);
    }
    template<bool checked>
    inline void dds(uint32_t ctrl, uint32_t op)
    {
        pulse<checked>(Bits::DDS | ctrl, op);
    }

public:
    static constexpr uint32_t max_wait_t = (1 << 24) - 1;
    // Public functions that are not exposed by the dummy pulser.
    // set bytes at addr + 1 and addr
    template<bool checked>
    inline void dds_set_2bytes(int i, uint32_t addr, uint32_t data)
    {
        // put addr in bits 15...9 (maps to DDS opcode_reg[14:9])?
        // put data in bits 15...0 (maps to DDS operand_reg[15:0])?
        dds<checked>(0x2 | (i << 4) | (((addr + 1) & 0x7f) << 9), data & 0xffff);
    }
    // set bytes addr + 3 ... addr
    template<bool checked>
    inline void dds_set_4bytes(int i, uint32_t addr, uint32_t data)
    {
        // put addr in bits 15...9 (maps to DDS opcode_reg[14:9])?
        dds<checked>(0xf | (i << 4) | (((addr + 1) & 0x7f) << 9), data);
    }
    template<bool checked>
    inline void dds_get_2bytes(int i, uint32_t addr)
    {
        dds<checked>(0x3 | (i << 4) | ((addr + 1) << 9), 0);
    }
    template<bool checked>
    inline void dds_get_4bytes(int i, uint32_t addr)
    {
        dds<checked>(0xe | (i << 4) | ((addr + 1) << 9), 0);
    }

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
    inline uint32_t cur_ttl() const
    {
        return read(4);
    }
    inline uint8_t cur_clock() const
    {
        return (uint8_t)read(5);
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
    inline void ttl(uint32_t ttl, uint32_t t)
    {
        assert(t <= max_wait_t);
        pulse<checked>(t, ttl);
    }
    template<bool checked>
    inline void clock(uint8_t div)
    {
        pulse<checked>(Bits::ClockOut, div & 0xff);
    }
    template<bool checked>
    inline void dac(uint8_t dac, uint16_t V)
    {
        spi<checked>(0, 0, ((dac & 3) << 16) | V);
    }
    template<bool checked>
    inline void wait(uint32_t t)
    {
        assert(t <= max_wait_t);
        pulse<checked>(Bits::Wait | t, 0);
    }
    // clear timing check (clear failures)
    inline void clear_error()
    {
        pulse<false>(Bits::ClearErr, 0);
    }
    template<bool checked>
    inline void dds_set_freq(int i, uint32_t ftw)
    {
        dds<checked>(i << 4, ftw);
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
        dds<checked>(0x4 | (i << 4), 0);
    }

    // Pulses with results
    // clear timing check (clear failures)
    template<bool checked>
    inline void loopback(uint32_t data)
    {
        pulse<checked>(Bits::LoopBack, data);
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

    // Debug registers
    inline uint32_t loopback_reg()
    {
        return read(0x1e);
    }
    inline void set_loopback_reg(uint32_t val)
    {
        write(0x1e, val);
    }
    inline uint32_t inst_word_count()
    {
        return read(0x20);
    }
    inline uint32_t inst_count()
    {
        return read(0x21);
    }

    Pulser(volatile void *const addr)
        : m_addr(addr)
    {}
    Pulser(Pulser &&other)
        : m_addr(other.m_addr)
    {}

    // Read a result if one is available. Return whether a result is returned in `res`.
    bool try_get_result(uint32_t &res);
    // Wait for a result to be available and return it.
    uint32_t get_result();

    // Initialize a dds channel. Including initializing the right mode,
    // clearing unused registers and setting the magic bytes which we'll check later.
    void init_dds(int chn);
    // Check if the DDS is in good shape.
    // If the magic bytes isn't set or if `force` is `true`,
    // (re)initialize the DDS channel. Returns whether a initialization is done.
    bool check_dds(int chn, bool force);
    // Check if the DDS exists
    // (by flip/flopping a register and see if the DDS responses correctly)
    bool dds_exists(int chn);
    // Print all the non-zero word (4-bytes) in the DDS memory.
    void dump_dds(std::ostream &stm, int chn);

    static void *address();

private:
    static constexpr uint32_t magic_bytes = 0xf00f0000;
    volatile void *const m_addr;
};

}

#endif
