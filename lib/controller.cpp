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

namespace Molecube {

template<typename Pulser>
class Controller<Pulser>::Runner {
public:
    Runner(Controller &ctrl, uint32_t ttlmask)
        : m_ctrl(ctrl),
          m_ttlmask(ttlmask)
    {
        m_ttl = ctrl.m_p.cur_ttl();
        m_preserve_ttl = (~ttlmask) & m_ttl;
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
            m_ctrl.m_p.template ttl<true>(m_ttl, (uint32_t)t);
            m_t += t;
        }
        else {
            m_ctrl.m_p.template ttl<true>(m_ttl, 100);
            m_t += 100;
            wait(t - 100);
        }
    }
    void dds_freq(uint8_t chn, uint32_t freq)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].freq != uint32_t(-1))) {
            wait(50);
            return;
        }
        m_ctrl.m_p.template dds_set_freq<true>(chn, freq);
        m_t += 50;
    }
    void dds_amp(uint8_t chn, uint16_t amp)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].amp_enable)) {
            wait(50);
            return;
        }
        m_ctrl.m_p.template dds_set_amp<true>(chn, amp);
        m_t += 50;
    }
    void dds_phase(uint8_t chn, uint16_t phase)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].phase_enable)) {
            wait(50);
            return;
        }
        m_ctrl.m_dds_phase[chn] = phase;
        m_ctrl.m_p.template dds_set_phase<true>(chn, phase);
        m_t += 50;
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
        m_ctrl.m_dds_ovr[chn].phase_enable = 0;
        m_ctrl.m_dds_ovr[chn].amp_enable = 0;
        m_ctrl.m_dds_ovr[chn].freq = -1;
        m_ctrl.m_p.template dds_reset<true>(chn);
        m_t += 50;
    }
    void dac(uint8_t chn, uint16_t V)
    {
        m_ctrl.m_p.template dac<true>(chn, V);
        m_t += 45;
    }
    void wait(uint64_t t)
    {
        // TODO
    }
    void clock(uint8_t period)
    {
        m_ctrl.m_p.template clock<true>(period);
        m_t += 5;
    }

private:
    Controller &m_ctrl;
    uint32_t m_ttlmask;
    uint32_t m_ttl;
    uint32_t m_preserve_ttl;
    uint64_t m_t;
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
