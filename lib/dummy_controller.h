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

#include <vector>
#include <functional>

namespace Molecube {

class DummyController {
    DummyController(const DummyController&) = delete;
    void operator=(const DummyController&) = delete;
public:
    DummyController();
    uint64_t runByteCode(uint64_t seq_len_ns, uint32_t ttl_mask,
                         const uint8_t *code, size_t code_len,
                         std::function<void()> seq_done);
    uint64_t runCmdList(uint64_t seq_len_ns, uint32_t ttl_mask,
                        const uint8_t *code, size_t code_len,
                        std::function<void()> seq_done);
    std::pair<uint32_t,uint32_t> overwriteTTL(uint32_t hi, uint32_t lo, uint32_t norm);
    uint32_t setTTL(uint32_t hi, uint32_t lo);

    void overwriteDDS(DDS::Info *infos, size_t ninfo);
    void setDDS(DDS::Info *infos, size_t ninfo);

    std::vector<DDS::Info> getOverwriteDDS();
    std::vector<DDS::Info> getDDS(uint8_t *nums, size_t nnum);
    std::vector<DDS::Info> getDDS()
    {
        return getDDS(nullptr, 0);
    }
private:
    static constexpr uint8_t NDDS = 22;
    uint64_t m_cur_t;
    uint32_t m_ttl = 0;
    uint8_t m_clock = 255;
    uint32_t m_dds_freqs[NDDS] = {0};
    uint16_t m_dds_amps[NDDS] = {0};
    uint16_t m_dds_phases[NDDS] = {0};
    // TODO: DAC
};

}

#endif
