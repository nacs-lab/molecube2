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

#ifndef LIBMOLECUBE_DUMMY_CONTROLLER_H
#define LIBMOLECUBE_DUMMY_CONTROLLER_H

#include "ctrl_iface.h"

#include <atomic>
#include <vector>

namespace Molecube {

class DummyController : public CtrlIFace {
    DummyController(const DummyController&) = delete;
    void operator=(const DummyController&) = delete;
public:
    DummyController();

private:
    bool concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t val) override;
    bool concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t &val) override;

    static constexpr uint8_t NDDS = 22;
    uint64_t m_cur_t;
    std::atomic<uint32_t> m_ttl{0};
    std::atomic<uint32_t> m_ttl_ovrlo{0};
    std::atomic<uint32_t> m_ttl_ovrhi{0};
    std::atomic<uint8_t> m_clock{255};
    uint32_t m_dds_freqs[NDDS] = {0};
    uint16_t m_dds_amps[NDDS] = {0};
    uint16_t m_dds_phases[NDDS] = {0};
};

}

#endif
