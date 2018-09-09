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
#include "dummy_pulser.h"

#include <nacs-utils/mem.h>

#include <vector>

namespace Molecube {

template<typename Pulser>
class Controller : public CtrlIFace {
    Controller(const Controller&) = delete;
    void operator=(const Controller&) = delete;

public:
    Controller(Pulser &&p);

private:
    class Runner;
    friend class Runner;

    bool concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t val) override;
    bool concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t &val) override;
    std::vector<int> get_active_dds() override;

    bool check_dds(int chn);
    // Process a command.
    // Return the sequence time forwarded and if the command needs a result.
    template<bool checked>
    std::pair<uint32_t,bool> run_cmd(const ReqCmd *cmd, Runner *runner=nullptr);

    static constexpr uint8_t NDDS = 22;

    Pulser m_p;
    DDSState m_dds_ovr[NDDS];
    uint16_t m_dds_phase[NDDS] = {0};
    // Reinitialize is a complicated sequence and is rarely needed
    // so only do that after the sequence finishes.
    bool m_dds_pending_reset[NDDS] = {false};
    bool m_dds_exist[NDDS] = {false};
    uint64_t m_dds_check_time;
    ReqCmd *m_cmd_waiting = nullptr;
};

extern template class Controller<Pulser>;
extern template class Controller<DummyPulser>;

}

#endif
