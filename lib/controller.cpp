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

#include "controller.h"

#include <nacs-utils/timer.h>

#include <chrono>
#include <iostream>
#include <tuple>

namespace Molecube {

template<typename Pulser>
class Controller<Pulser>::Runner {
public:
    Runner(Controller &ctrl, uint32_t ttlmask)
        : m_ctrl(ctrl),
          m_ttlmask(ttlmask),
          m_ttl(ctrl.m_p.cur_ttl()),
          m_preserve_ttl((~ttlmask) & m_ttl)
    {
    }
    void ttl1(uint8_t chn, bool val, uint64_t t)
    {
        ttl(setBit(m_ttl, chn, val), t);
    }
    void ttl(uint32_t ttl, uint64_t t)
    {
        m_ttl = ttl | m_preserve_ttl;
        if (t <= 1000) {
            // 10us
            m_t += t;
            m_ctrl.m_p.template ttl<true>(m_ttl, (uint32_t)t);
        }
        else {
            m_t += 100;
            m_ctrl.m_p.template ttl<true>(m_ttl, 100);
            wait(t - 100);
        }
    }
    void dds_freq(uint8_t chn, uint32_t freq)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].freq != uint32_t(-1))) {
            wait(50);
            return;
        }
        m_t += 50;
        m_ctrl.m_p.template dds_set_freq<true>(chn, freq);
    }
    void dds_amp(uint8_t chn, uint16_t amp)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].amp_enable)) {
            wait(50);
            return;
        }
        m_t += 50;
        m_ctrl.m_p.template dds_set_amp<true>(chn, amp);
    }
    void dds_phase(uint8_t chn, uint16_t phase)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].phase_enable)) {
            wait(50);
            return;
        }
        m_ctrl.m_dds_phase[chn] = phase;
        m_t += 50;
        m_ctrl.m_p.template dds_set_phase<true>(chn, phase);
    }
    void dds_detphase(uint8_t chn, uint16_t detphase)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].phase_enable)) {
            wait(50);
            return;
        }
        dds_phase(chn, uint16_t(m_ctrl.m_dds_phase[chn] + detphase));
    }
    void dds_reset(uint8_t chn)
    {
        auto &ovr = m_ctrl.m_dds_ovr[chn];
        ovr.phase_enable = 0;
        ovr.amp_enable = 0;
        ovr.freq = -1;
        m_ctrl.m_dds_phase[chn] = 0;
        m_t += 50;
        m_ctrl.m_p.template dds_reset<true>(chn);
    }
    void dac(uint8_t chn, uint16_t V)
    {
        m_t += 45;
        m_ctrl.m_p.template dac<true>(chn, V);
    }
    void clock(uint8_t period)
    {
        m_t += 5;
        m_ctrl.m_p.template clock<true>(period);
    }
    void wait(uint64_t t)
    {
        if (t < 1000) {
            // If the wait time is too short, don't do anything fancy
            m_t += t;
            m_ctrl.m_p.template wait<true>(uint32_t(t));
            return;
        }
        if (!m_released) {
            m_ctrl.m_p.release_hold();
            m_released = true;
        }
        const auto tend = m_t + t;
        auto tnow = getCoarseTime();
        while (true) {
            using namespace std::literals;
            if (m_start_t + m_t * 10 >= tnow + m_min_t) {
                // We have time to do something else
                uint32_t stept;
                bool processed;
                std::tie(stept, processed) = run_wait_step();
                m_t += stept;
                if (!processed) {
                    // Didn't find much to do. Sleep for a while
                    std::this_thread::sleep_for(0.2ms);
                }
            }
            else {
                // We need to do the actual sequence
                // At least forward the sequence for 1000 steps.
                auto stept = max((tnow + m_min_t - m_start_t) / 10 - m_t, 1000);
                stept = min(stept, tend - m_t);
                if (m_t + stept + 1000 <= tend) {
                    // If we are close to the end after this wait, just finish it up.
                    m_t = tend;
                    m_ctrl.m_p.template wait<true>(uint32_t(tend - m_t));
                    return;
                }
                m_t += stept;
                m_ctrl.m_p.template wait<true>(uint32_t(stept));
            }
            if (tend < m_t + 1000) {
                assert(m_t < tend);
                m_t = tend;
                m_ctrl.m_p.template wait<true>(uint32_t(tend - m_t));
                return;
            }
            tnow = getCoarseTime();
        }
    }

    // Try to process a command or result.
    // Return the sequence time forwarded and whether anything non-trivial is done.
    std::pair<uint32_t,bool> run_wait_step()
    {
        if (m_cmd_waiting) {
            if (m_ctrl.m_p.try_get_result(m_cmd_waiting->val)) {
                m_cmd_waiting = nullptr;
                m_ctrl.finish_cmd();
            }
            return {0, true};
        }
        if (auto cmd = m_ctrl.get_cmd()) {
            auto res = send_request(cmd);
            if (res.second)
                m_cmd_waiting = cmd;
            return {res.first, true};
        }
        return {0, false};
    }

    // Process a command.
    // Return the sequence time forwarded and if the command needs a result.
    std::pair<uint32_t,bool> send_request(const Controller::ReqCmd *cmd)
    {
        switch (cmd->opcode) {
        case Controller::TTL:
            // Should have been caught by concurrent_get/set.
            assert(!cmd->has_res && !cmd->is_override);
            if (cmd->operand == uint32_t((1 << 26) - 1)) {
                auto val = cmd->val;
                m_ttl = val;
                m_preserve_ttl = val & m_ttlmask;
            }
            else {
                assert(cmd->operand < 32);
                auto chn = uint8_t(cmd->operand);
                bool val = cmd->val;
                if (m_ttlmask & (1 << chn))
                    m_preserve_ttl = setBit(m_preserve_ttl, chn, val);
                m_ttl = setBit(m_ttl, chn, val);
            }
            m_ctrl.m_p.template ttl<true>(m_ttl, 3);
            return {3, false};
        case Controller::DDSFreq: {
            bool is_override = cmd->is_override;
            bool has_res = cmd->has_res;
            int chn = cmd->operand;
            uint32_t val = cmd->val;
            assert(chn < 22);
            auto &ovr = m_ctrl.m_dds_ovr[chn];
            if (!is_override && !has_res)
                is_override = true;
            if (is_override) {
                // Should be handled by the cache in ctrl_iface.
                assert(!has_res);
                if (val == ovr.freq) {
                    return {0, false};
                }
                ovr.freq = val;
                if (val == uint32_t(-1)) {
                    return {0, false};
                }
                else {
                    m_ctrl.m_p.template dds_set_freq<true>(chn, val);
                    return {50, false};
                }
            }
            if (!has_res) {
                m_ctrl.m_p.template dds_set_freq<true>(chn, val);
                return {50, false};
            }
            m_ctrl.m_p.template dds_get_freq<true>(chn);
            return {50, true};
        }
        case Controller::DDSAmp: {
            bool is_override = cmd->is_override;
            bool has_res = cmd->has_res;
            int chn = cmd->operand;
            uint16_t val = uint16_t(cmd->val);
            assert(chn < 22);
            auto &ovr = m_ctrl.m_dds_ovr[chn];
            if (!is_override && !has_res)
                is_override = true;
            if (is_override) {
                // Should be handled by the cache in ctrl_iface.
                assert(!has_res);
                if (val == ovr.amp) {
                    return {0, false};
                }
                else if (val == uint16_t(-1)) {
                    if (ovr.amp_enable)
                        ovr.amp_enable = false;
                    return {0, false};
                }
                else {
                    ovr.amp = uint16_t(val & ((1 << 12) - 1));
                    ovr.amp_enable = true;
                    m_ctrl.m_p.template dds_set_amp<true>(chn, val);
                    return {50, false};
                }
            }
            if (!has_res) {
                m_ctrl.m_p.template dds_set_amp<true>(chn, val);
                return {50, false};
            }
            m_ctrl.m_p.template dds_get_amp<true>(chn);
            return {50, true};
        }
        case Controller::DDSPhase: {
            bool is_override = cmd->is_override;
            bool has_res = cmd->has_res;
            int chn = cmd->operand;
            uint16_t val = uint16_t(cmd->val);
            assert(chn < 22);
            auto &ovr = m_ctrl.m_dds_ovr[chn];
            if (!is_override && !has_res)
                is_override = true;
            if (is_override) {
                // Should be handled by the cache in ctrl_iface.
                assert(!has_res);
                if (val == ovr.phase) {
                    return {0, false};
                }
                else if (val == uint16_t(-1)) {
                    if (ovr.phase_enable)
                        ovr.phase_enable = false;
                    return {0, false};
                }
                else {
                    ovr.phase = val;
                    ovr.phase_enable = true;
                    m_ctrl.m_dds_phase[chn] = val;
                    m_ctrl.m_p.template dds_set_phase<true>(chn, val);
                    return {50, false};
                }
            }
            if (!has_res) {
                m_ctrl.m_dds_phase[chn] = val;
                m_ctrl.m_p.template dds_set_phase<true>(chn, val);
                return {50, false};
            }
            m_ctrl.m_p.template dds_get_phase<true>(chn);
            return {50, true};
        }
        case Controller::DDSReset:
        case Controller::Clock:
            // TODO
        default:
            return {0, false};
        }
    }

private:
    Controller &m_ctrl;
    uint32_t m_ttlmask;
    uint32_t m_ttl;
    uint32_t m_preserve_ttl;
    uint64_t m_t{0};

    const uint64_t m_start_t{getCoarseTime()};
    const uint64_t m_min_t{max(getCoarseRes() * 3, 3000000)};

    bool m_released = false;
    Controller::ReqCmd *m_cmd_waiting = nullptr;
};

template<typename Pulser>
Controller<Pulser>::Controller(Pulser &&p)
    : m_p(std::move(p))
{
    for (int i = 0; i < NDDS; i++) {
        if (!m_p.dds_exists(i)) {
            m_dds_exist[i] = false;
            continue;
        }
        m_dds_exist[i] = true;
        if (check_dds(i)) {
            std::cerr << "DDS " << i << " initialized" << std::endl;
        }
        else {
            m_p.template dds_reset<false>(i);
        }
        m_p.dump_dds(std::cerr, i);
    }
    m_p.clear_error();
    m_dds_check_time = getTime();
}

template<typename Pulser>
bool Controller<Pulser>::concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                                        uint32_t val)
{
    if (op != TTL)
        return false;
    if (!is_override)
        return false;
    if (operand == 0) {
        m_p.set_ttl_lomask(val);
        return true;
    }
    else if (operand == 1) {
        m_p.set_ttl_himask(val);
        return true;
    }
    return false;
}

template<typename Pulser>
bool Controller<Pulser>::concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                                        uint32_t &val)
{
    if (op == Clock) {
        val = m_p.cur_clock();
        return false;
    }
    if (op != TTL)
        return false;
    if (!is_override) {
        if (operand != 0)
            return false;
        val = (m_p.cur_ttl() | m_p.ttl_himask()) & ~m_p.ttl_lomask();
        return true;
    }
    if (operand == 0) {
        val = m_p.ttl_lomask();
        return true;
    }
    else if (operand == 1) {
        val = m_p.ttl_himask();
        return true;
    }
    return false;
}

template<typename Pulser>
bool Controller<Pulser>::check_dds(int chn)
{
    auto res = m_p.check_dds(chn, m_dds_pending_reset[chn]);
    m_dds_pending_reset[chn] = false;
    return res;
}

template<typename Pulser>
std::vector<int> Controller<Pulser>::get_active_dds()
{
    std::vector<int> res;
    for (int i = 0; i < NDDS; i++) {
        if (m_dds_exist[i]) {
            res.push_back(i);
        }
    }
    return res;
}

template class Controller<Pulser>;
template class Controller<DummyPulser>;

NACS_PROTECTED() std::unique_ptr<CtrlIFace> CtrlIFace::create(bool dummy)
{
    if (!dummy) {
        if (auto addr = Molecube::Pulser::address())
            return std::unique_ptr<CtrlIFace>(new Controller<Pulser>(Pulser(addr)));
        fprintf(stderr, "Warning: failed to create real pulser, use dummy pulser instead.");
    }
    return std::unique_ptr<CtrlIFace>(new Controller<DummyPulser>(DummyPulser()));
}

}
