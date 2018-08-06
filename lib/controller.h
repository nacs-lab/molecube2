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
        return !(read(2) & 0x1);
    }
    inline bool is_finished() const
    {
        return read(2) & 0x4;
    }
    inline uint32_t num_results() const
    {
        return (read(2) >> 4) & 31;
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
        write(3, read(3) & ~0x80);
    }
    // set hold. pulses are stopped
    inline void set_hold()
    {
        write(3, read(3) | 0x80);
    }
    // toggle init. reset prior to new sequence
    inline void toggle_init()
    {
        uint32_t r3 = read(3);
        write(3, r3 | 0x00000100);
        write(3, r3 & ~0x00000100);
    }
    inline void short_pulse(uint32_t ctrl, uint32_t op)
    {
        write(31, op);
        write(31, ctrl);
    }
public:
    Controller();

private:
    bool concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t val) override;
    bool concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t &val) override;

    static constexpr uint8_t NDDS = 22;

    volatile void *const m_addr;
    DDSState m_dds_ovr[NDDS];
};

}

#endif
