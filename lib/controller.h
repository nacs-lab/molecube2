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
    inline void write(uint32_t reg, uint32_t val) const
    {
        Mem::write<uint32_t>(m_addr, val, reg);
    }

    // Read
    inline uint32_t getTTLHighMask() const
    {
        return read(0);
    }
    inline uint32_t getTTLLowMask() const
    {
        return read(1);
    }
    inline bool timingOK() const
    {
        return !(read(2) & 0x1);
    }
    inline bool isFinished() const
    {
        return read(2) & 0x4;
    }
    inline uint32_t numResults() const
    {
        return (read(2) >> 4) & 31;
    }
    inline uint32_t getCurTTL() const
    {
        return read(4);
    }
    inline uint32_t popResult() const
    {
        return read(31);
    }

    // Write
    // TTL functions: pulse_io = (ttl_out | high_mask) & (~low_mask);
    inline void setTTLHighMask(uint32_t high_mask) const
    {
        write(0, high_mask);
    }
    inline void setTTLLowMask(uint32_t low_mask) const
    {
        write(1, low_mask);
    }
    // release hold.  pulses can run
    inline void releaseHold()
    {
        write(3, read(3) & ~0x80);
    }
    // set hold. pulses are stopped
    inline void setHold()
    {
        write(3, read(3) | 0x80);
    }
    // toggle init. reset prior to new sequence
    inline void toggleInit()
    {
        uint32_t r3 = read(3);
        write(3, r3 | 0x00000100);
        write(3, r3 & ~0x00000100);
    }
    inline void shortPulse(uint32_t ctrl, uint32_t op) const
    {
        write(31, op);
        write(31, ctrl);
    }
public:
    Controller();
private:
    volatile void *const m_addr;
};

}

#endif
