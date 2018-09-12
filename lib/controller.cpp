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

#include "ctrl_iface.h"
#include "pulser.h"
#include "dummy_pulser.h"

#include <nacs-utils/container.h>
#include <nacs-utils/mem.h>
#include <nacs-utils/timer.h>

#include <nacs-seq/bytecode.h>
#include <nacs-seq/cmdlist.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <tuple>

namespace {
using namespace Molecube;

template<typename Pulser>
class Controller : public CtrlIFace {
    Controller(const Controller&) = delete;
    void operator=(const Controller&) = delete;

public:
    Controller(Pulser &&p);
    ~Controller();

private:
    class Runner;
    friend class Runner;

    bool concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t val) override;
    bool concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t &val) override;
    std::vector<int> get_active_dds() override;

    bool check_dds(int chn);
    void detect_dds(bool force=false);

    // Process a command.
    // Returns the sequence time forwarded and if the command needs a result.
    template<bool checked>
    std::pair<uint32_t,bool> run_cmd(const ReqCmd *cmd, Runner *runner=nullptr);
    // Check if we are waiting for results. If yes, try to get one.
    // Returns whether anything non-trivial is done, and whether a result was read.
    template<bool checked>
    std::pair<bool,bool> try_get_result();
    // Try to process a command or result.
    // Returns the sequence time forwarded and whether anything non-trivial is done.
    template<bool checked>
    std::pair<uint32_t,bool> process_reqcmd(Runner *runner=nullptr);

    void run_seq(ReqSeq *seq);

    void worker();

    static constexpr uint8_t NDDS = 22;

    Pulser m_p;
    DDSState m_dds_ovr[NDDS];
    uint16_t m_dds_phase[NDDS] = {0};
    // Reinitialize is a complicated sequence and is rarely needed
    // so only do that after the sequence finishes.
    bool m_dds_pending_reset[NDDS] = {false};
    std::atomic<bool> m_dds_exist[NDDS] = {};
    uint64_t m_dds_check_time = 0;
    FixedQueue<ReqCmd*,16> m_cmd_waiting;

    std::thread m_worker;
};

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
        // Do the reset pulse that's part of the sequence but do the
        // actual reinitialization later after the sequence finishes.
        m_t += 50;
        m_ctrl.m_p.template dds_reset<true>(chn);
        m_ctrl.m_dds_pending_reset[chn] = true;
    }
    void dac(uint8_t chn, uint16_t V)
    {
        m_t += 45;
        m_ctrl.m_p.template dac<true>(chn, V);
    }
    template<bool checked=true>
    void clock(uint8_t period)
    {
        m_t += 5;
        m_ctrl.m_p.template clock<checked>(period);
    }
    template<bool checked=true>
    void wait(uint64_t t)
    {
        if (t < 1000) {
            // If the wait time is too short, don't do anything fancy
            m_t += t;
            m_ctrl.m_p.template wait<checked>(uint32_t(t));
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
                std::tie(stept, processed) = m_ctrl.process_reqcmd<checked>(this);
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
                    m_ctrl.m_p.template wait<checked>(uint32_t(tend - m_t));
                    return;
                }
                m_t += stept;
                m_ctrl.m_p.template wait<checked>(uint32_t(stept));
            }
            if (tend < m_t + 1000) {
                assert(m_t < tend);
                m_t = tend;
                m_ctrl.m_p.template wait<checked>(uint32_t(tend - m_t));
                return;
            }
            tnow = getCoarseTime();
        }
    }

    Controller &m_ctrl;
    const uint32_t m_ttlmask;
    uint32_t m_ttl;
    uint32_t m_preserve_ttl;
private:
    uint64_t m_t{0};

    const uint64_t m_start_t{getCoarseTime()};
    const uint64_t m_min_t{max(getCoarseRes() * 3, 3000000)};

    bool m_released = false;
};

template<typename Pulser>
Controller<Pulser>::Controller(Pulser &&p)
    : m_p(std::move(p)),
      m_worker(&Controller<Pulser>::worker, this)
{
    detect_dds(true);
    m_p.clear_error();
}

template<typename Pulser>
Controller<Pulser>::~Controller()
{
    quit();
    m_worker.join();
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
    if (m_dds_pending_reset[chn]) {
        auto &ovr = m_dds_ovr[chn];
        ovr.phase_enable = 0;
        ovr.amp_enable = 0;
        ovr.freq = -1;
        m_dds_phase[chn] = 0;
    }
    auto res = m_p.check_dds(chn, m_dds_pending_reset[chn]);
    m_dds_pending_reset[chn] = false;
    return res;
}

template<typename Pulser>
void Controller<Pulser>::detect_dds(bool force)
{
    const auto t = getCoarseTime();
    auto has_pending_reset = [&] () {
        for (auto v: m_dds_pending_reset) {
            if (v) {
                return true;
            }
        }
        return false;
    };
    if (!force && t < m_dds_check_time + 1000000000 && !has_pending_reset())
        return;
    for (int i = 0; i < NDDS; i++) {
        if (!m_p.dds_exists(i)) {
            m_dds_exist[i].store(false, std::memory_order_relaxed);
            m_dds_pending_reset[i] = false;
            continue;
        }
        m_dds_exist[i].store(true, std::memory_order_relaxed);
        if (force)
            m_dds_pending_reset[i] = true;
        if (check_dds(i) && force)
            std::cerr << "DDS " << i << " initialized" << std::endl;
        if (force) {
            m_p.dump_dds(std::cerr, i);
        }
    }
    m_dds_check_time = t;
}

template<typename Pulser>
std::vector<int> Controller<Pulser>::get_active_dds()
{
    std::vector<int> res;
    for (int i = 0; i < NDDS; i++) {
        if (m_dds_exist[i].load(std::memory_order_relaxed)) {
            res.push_back(i);
        }
    }
    return res;
}

template<typename Pulser>
template<bool checked>
std::pair<uint32_t,bool> Controller<Pulser>::run_cmd(const ReqCmd *cmd, Runner *runner)
{
    switch (cmd->opcode) {
    case TTL: {
        // Should have been caught by concurrent_get/set.
        assert(!cmd->has_res && !cmd->is_override);
        uint32_t ttl;
        if (cmd->operand == uint32_t((1 << 26) - 1)) {
            ttl = cmd->val;
            if (runner) {
                runner->m_ttl = ttl;
                runner->m_preserve_ttl = ttl & runner->m_ttlmask;
            }
        }
        else {
            assert(cmd->operand < 32);
            auto chn = uint8_t(cmd->operand);
            bool val = cmd->val;
            if (runner) {
                if (runner->m_ttlmask & (1 << chn))
                    runner->m_preserve_ttl = setBit(runner->m_preserve_ttl, chn, val);
                ttl = runner->m_ttl;
            }
            else {
                ttl = m_p.cur_ttl();
            }
            ttl = setBit(ttl, chn, val);
        }
        m_p.template ttl<checked>(ttl, 3);
        return {3, false};
    }
    case DDSFreq: {
        bool is_override = cmd->is_override;
        bool has_res = cmd->has_res;
        int chn = cmd->operand;
        uint32_t val = cmd->val;
        assert(chn < 22);
        auto &ovr = m_dds_ovr[chn];
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
                m_p.template dds_set_freq<checked>(chn, val);
                return {50, false};
            }
        }
        if (!has_res) {
            m_p.template dds_set_freq<checked>(chn, val);
            return {50, false};
        }
        m_p.template dds_get_freq<checked>(chn);
        return {50, true};
    }
    case DDSAmp: {
        bool is_override = cmd->is_override;
        bool has_res = cmd->has_res;
        int chn = cmd->operand;
        uint16_t val = uint16_t(cmd->val);
        assert(chn < 22);
        auto &ovr = m_dds_ovr[chn];
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
                m_p.template dds_set_amp<checked>(chn, val);
                return {50, false};
            }
        }
        if (!has_res) {
            m_p.template dds_set_amp<checked>(chn, val);
            return {50, false};
        }
        m_p.template dds_get_amp<checked>(chn);
        return {50, true};
    }
    case DDSPhase: {
        bool is_override = cmd->is_override;
        bool has_res = cmd->has_res;
        int chn = cmd->operand;
        uint16_t val = uint16_t(cmd->val);
        assert(chn < 22);
        auto &ovr = m_dds_ovr[chn];
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
                m_dds_phase[chn] = val;
                m_p.template dds_set_phase<checked>(chn, val);
                return {50, false};
            }
        }
        if (!has_res) {
            m_dds_phase[chn] = val;
            m_p.template dds_set_phase<checked>(chn, val);
            return {50, false};
        }
        m_p.template dds_get_phase<checked>(chn);
        return {50, true};
    }
    case DDSReset: {
        assert(!cmd->is_override && !cmd->has_res && cmd->val == 0);
        int chn = cmd->operand;
        assert(chn < 22);
        m_dds_pending_reset[chn] = true;
        return {0, false};
    }
    case Clock:
        assert(!cmd->is_override && !cmd->has_res && cmd->operand == 0);
        m_p.template clock<checked>(uint8_t(cmd->val));
        return {5, false};
    default:
        return {0, false};
    }
}

template<typename Pulser>
template<bool checked>
std::pair<bool,bool> Controller<Pulser>::try_get_result()
{
    if (!m_cmd_waiting.empty()) {
        if (!m_p.try_get_result(m_cmd_waiting.front()->val))
            return {true, false};
        m_cmd_waiting.pop();
        finish_cmd();
        if (!checked) {
            // The time is not very important, notify the frontend.
            backend_event();
        }
        return {true, true};
    }
    return {false, false};
}

template<typename Pulser>
template<bool checked>
std::pair<uint32_t,bool> Controller<Pulser>::process_reqcmd(Runner *runner)
{
    bool processed;
    bool res_read;
    std::tie(processed, res_read) = try_get_result<checked>();
    if (res_read)
        return {0, true};
    if (auto cmd = get_cmd()) {
        // If the command potentially have a result
        // and if the result queue is already full, wait until the queue has space.
        if (cmd->has_res && m_cmd_waiting.full())
            return {0, true};
        auto res = run_cmd<checked>(cmd, runner);
        if (res.second) {
            m_cmd_waiting.push(cmd);
        }
        else {
            finish_cmd();
            if (!checked) {
                // The time is not very important, notify the frontend.
                backend_event();
            }
        }
        return {res.first, true};
    }
    return {0, processed};
}

template<typename Pulser>
void Controller<Pulser>::run_seq(ReqSeq *seq)
{
    // Read all the result (`toggle_init` may abort it).
    while (true) {
        auto res = try_get_result<false>();
        if (!res.first)
            break;
        if (unlikely(!res.second)) {
            std::this_thread::yield();
        }
    }
    // Make sure all commands are finished (`toggle_init` will clear them)
    while (unlikely(!m_p.is_finished()))
        std::this_thread::yield();
    m_p.set_hold();
    // `toggle_init` is needed to clear the force release flag
    // so that `set_hold` can work.
    m_p.toggle_init();
    seq->state.store(SeqStart, std::memory_order_relaxed);
    backend_event();

    Runner runner(*this, seq->ttl_mask);
    if (unlikely(seq->is_cmd)) {
        Seq::CmdList::ExeState exestate;
        exestate.run(runner, seq->code, seq->code_len);
    }
    else {
        Seq::ByteCode::ExeState exestate;
        exestate.run(runner, seq->code, seq->code_len);
    }
    m_p.release_hold();
    // Stop the timing check with a short wait.
    runner.template wait<false>(3);
    seq->state.store(SeqFlushed, std::memory_order_relaxed);
    backend_event();
    if (!seq->is_cmd) {
        // This is a hack that is believed to make the NI card happy.
        runner.template clock<false>(9);
    }
    // Wait for the sequence to finish.
    while (!m_p.is_finished()) {
        if (!process_reqcmd<false>(&runner).second) {
            std::this_thread::yield();
        }
    }
    seq->state.store(SeqEnd, std::memory_order_relaxed);
    backend_event();
    if (!seq->is_cmd) {
        // 10ms
        runner.template wait<false>(1000000);
        runner.template clock<false>(255);
    }
    if (!m_p.timing_ok())
        std::cerr << "Warning: timing failures." << std::endl;
    m_p.clear_error();

    // Doing this check before this sequence will make the current sequence
    // more likely to work. However, that increase the latency and the DDS
    // reset only happen very infrequently so let's do it after the sequence
    // for better efficiency.
    for (int i = 0; i < NDDS; i++) {
        if (m_dds_exist[i].load(std::memory_order_relaxed) && check_dds(i)) {
            std::cerr << "DDS " << i << " reinit" << std::endl;
            m_p.dump_dds(std::cerr, i);
        }
    }
}

template<typename Pulser>
void Controller<Pulser>::worker()
{
    while (wait(500000000)) { // Wake up every 500ms
        if (auto seq = get_seq()) {
            run_seq(seq);
            finish_seq();
        }
        detect_dds();
        process_reqcmd<false>();
        detect_dds();
    }
}

} // anonymous namespace

namespace Molecube {

NACS_PROTECTED() std::unique_ptr<CtrlIFace> CtrlIFace::create(bool dummy)
{
    if (!dummy) {
        if (auto addr = Molecube::Pulser::address())
            return std::unique_ptr<CtrlIFace>(new Controller<Pulser>(Pulser(addr)));
        std::cerr << "Warning: failed to create real pulser, use dummy pulser instead."
                  << std::endl;
    }
    return std::unique_ptr<CtrlIFace>(new Controller<DummyPulser>(DummyPulser()));
}

}
