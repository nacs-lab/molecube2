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
#include "pulser.h"

#include <nacs-utils/mem.h>

#include <vector>
#include <ostream>

namespace Molecube {

class Controller : public CtrlIFace, Pulser {
    Controller(const Controller&) = delete;
    void operator=(const Controller&) = delete;

public:
    Controller();

private:
    class Runner;
    friend class Runner;

    bool concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t val) override;
    bool concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t &val) override;
    std::vector<int> get_active_dds() override;

    bool try_get_result(uint32_t &res);
    uint32_t get_result();

    void init_dds(int chn);
    bool check_dds(int chn);
    bool dds_exists(int chn);
    void dump_dds(std::ostream &stm, int chn);

    static constexpr uint8_t NDDS = 22;
    static constexpr uint32_t magic_bytes = 0xf00f0000;

    DDSState m_dds_ovr[NDDS];
    uint16_t m_dds_phase[NDDS] = {0};
    // Reinitialize is a complicated sequence and is rarely needed
    // so only do that after the sequence finishes.
    bool m_dds_pending_reset[NDDS] = {false};
    bool m_dds_exist[NDDS] = {false};
    uint64_t m_dds_check_time;
};

}

#endif
