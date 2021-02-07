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
#include <nacs-utils/log.h>
#include <nacs-utils/mem.h>
#include <nacs-utils/streams.h>
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

    bool concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t val) override;
    bool concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                        uint32_t &val) override;
    std::vector<int> get_active_dds() override;
    bool has_ttl_ovr() override;

    bool check_dds(int chn);
    void detect_dds(bool force=false);
    void dump_dds(int i);

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

    void sync_ttl()
    {
        // This function shouldn't be necessary if we did everything correctly.
        // This is called periodically in the main loop and also before the sequence start.
        // to make sure we don't accumulate errors even if we failed to keep track of
        // every changes.
        auto ttl = m_p.cur_ttl();
        if (ttl != m_ttl) {
            Log::warn("TTL out of sync: has %04x, actual %04x\n", m_ttl, ttl);
            m_ttl = ttl;
        }
    }

    static constexpr uint8_t NDDS = 22;

    Pulser m_p;
    DDSState m_dds_ovr[NDDS];
    uint32_t m_ttl;
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
    Runner(Controller &ctrl, uint32_t ttlmask, uint64_t seq_len_ns)
        : m_ctrl(ctrl),
          m_ttlmask(ttlmask),
          m_preserve_ttl((~ttlmask) & ctrl.m_ttl),
          m_process_cmd(seq_len_ns > 1000000000ul) // 1s
    {
    }
    void ttl1(uint8_t chn, bool val, uint64_t t)
    {
        ttl(setBit(m_ctrl.m_ttl, chn, val), t);
    }
    void ttl(uint32_t ttl, uint64_t t)
    {
        m_ctrl.m_ttl = ttl | m_preserve_ttl;
        if (t <= 1000) {
            // 10us
            m_t += t;
            m_ctrl.m_p.template ttl<true>(m_ctrl.m_ttl, (uint32_t)t);
        }
        else {
            m_t += 100;
            m_ctrl.m_p.template ttl<true>(m_ctrl.m_ttl, 100);
            wait(t - 100);
        }
    }
    void dds_freq(uint8_t chn, uint32_t freq)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].freq != uint32_t(-1))) {
            wait(Seq::PulseTime::DDSFreq);
            return;
        }
        m_t += Seq::PulseTime::DDSFreq;
        m_ctrl.m_p.template dds_set_freq<true>(chn, freq);
    }
    void dds_amp(uint8_t chn, uint16_t amp)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].amp_enable)) {
            wait(Seq::PulseTime::DDSAmp);
            return;
        }
        m_t += Seq::PulseTime::DDSAmp;
        m_ctrl.m_p.template dds_set_amp<true>(chn, amp);
    }
    void dds_phase(uint8_t chn, uint16_t phase)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].phase_enable)) {
            wait(Seq::PulseTime::DDSPhase);
            return;
        }
        m_ctrl.m_dds_phase[chn] = phase;
        m_t += Seq::PulseTime::DDSPhase;
        m_ctrl.m_p.template dds_set_phase<true>(chn, phase);
    }
    void dds_detphase(uint8_t chn, uint16_t detphase)
    {
        if (unlikely(m_ctrl.m_dds_ovr[chn].phase_enable)) {
            wait(Seq::PulseTime::DDSPhase);
            return;
        }
        dds_phase(chn, uint16_t(m_ctrl.m_dds_phase[chn] + detphase));
    }
    void dds_reset(uint8_t chn)
    {
        // Do the reset pulse that's part of the sequence but do the
        // actual reinitialization later after the sequence finishes.
        m_t += Seq::PulseTime::DDSReset;
        m_ctrl.m_p.template dds_reset<true>(chn);
        m_ctrl.m_dds_pending_reset[chn] = true;
    }
    void dac(uint8_t chn, uint16_t V)
    {
        m_t += Seq::PulseTime::DAC;
        m_ctrl.m_p.template dac<true>(chn, V);
    }
    template<bool checked=true>
    void clock(uint8_t period)
    {
        m_t += Seq::PulseTime::Clock;
        m_ctrl.m_p.template clock<checked>(period);
    }
    template<bool checked=true>
    void wait(uint64_t t)
    {
        auto output_wait = [&] (uint64_t t) {
            m_t += t;
            while (t > m_ctrl.m_p.max_wait_t + 100) {
                t -= m_ctrl.m_p.max_wait_t;
                m_ctrl.m_p.template wait<checked>(m_ctrl.m_p.max_wait_t);
            }
            if (t > m_ctrl.m_p.max_wait_t) {
                auto t0 = t / 2;
                m_ctrl.m_p.template wait<checked>(uint32_t(t0));
                m_ctrl.m_p.template wait<checked>(uint32_t(t - t0));
            }
            else if (t > 0) {
                m_ctrl.m_p.template wait<checked>(uint32_t(t));
            }
        };
        if (!m_process_cmd) {
            // The sequence is short enough that we can let the web page wait.
            output_wait(t);
            return;
        }
        if (t < 2000) {
            // If the wait time is too short, don't do anything fancy
            m_t += t;
            m_ctrl.m_p.template wait<checked>(uint32_t(t));
            return;
        }
        while (true) {
            // Now we always make sure that the sequence time is at least 0.5s ahead of
            // the real time.
            auto tnow = getCoarseTime();
            // Current sequence time in real time.
            auto seq_rt = m_start_t + m_t * 10;
            // We need to output to this time before processing commands.
            auto thresh_rt = tnow + m_min_t;
            if (seq_rt < thresh_rt) {
                auto min_seqt = max((seq_rt - thresh_rt) / 10, 10000);
                if (t <= min_seqt + 3000) {
                    output_wait(t);
                    return;
                }
                output_wait(min_seqt);
                t -= min_seqt;
                continue;
            }
            if (unlikely(!m_released)) {
                m_released = true;
                // At the beginning of the loop, `t` may come froms
                // 1. The value before entering the loop, in which case `t >= 2000`.
                // 2. `continue` for the `seq_rt < thresh_rt` case above, in which case
                //    `t >= 3000`.
                // 3. End of the loop. `t` could be less than `2000` in this case
                //    but there must be `m_release == true` and it won't end up in
                //    this branch again.
                assert(t >= 2000);
                m_t += 1000;
                m_ctrl.m_p.template wait<checked>(uint32_t(1000));
                t -= 1000;
                m_ctrl.m_p.release_hold();
            }
            // We have time to do something else
            uint32_t stept;
            bool processed;
            std::tie(stept, processed) = m_ctrl.process_reqcmd<checked>(this);
            if (!processed) {
                // Didn't find much to do. Sleep for a while
                using namespace std::literals;
                std::this_thread::sleep_for(1ms);
            }
            else {
                m_t += stept;
                t -= stept;
            }
        }
    }
    void update_preserve_ttl(uint32_t ttl)
    {
        m_preserve_ttl = ttl & m_ttlmask;
    }
    void enable_process_cmd()
    {
        m_process_cmd = true;
    }

private:
    Controller &m_ctrl;
    const uint32_t m_ttlmask;
    uint32_t m_preserve_ttl;
    uint64_t m_t{0};

    const uint64_t m_start_t{getCoarseTime()};
    // Minimum time we stay ahead of the sequence.
    const uint64_t m_min_t{max(getCoarseRes() * 20, 500000000)}; // 0.5s
    bool m_process_cmd;

    bool m_released = false;
};

template<typename Pulser>
Controller<Pulser>::Controller(Pulser &&p)
    : m_p(std::move(p)),
      m_ttl(m_p.cur_ttl()),
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
    if (op != TTL || !is_override)
        return false;
    auto lomask = m_p.ttl_lomask();
    auto himask = m_p.ttl_himask();
    if (operand == 0) {
        m_p.set_ttl_lomask((lomask | val));
        m_p.set_ttl_himask((himask & ~val));
    }
    else if (operand == 1) {
        m_p.set_ttl_lomask((lomask & ~val));
        m_p.set_ttl_himask((himask | val));
    }
    else if (operand == 2) {
        m_p.set_ttl_lomask((lomask & ~val));
        m_p.set_ttl_himask((himask & ~val));
    }
    else {
        return false;
    }
    return true;
}

template<typename Pulser>
bool Controller<Pulser>::concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                                        uint32_t &val)
{
    if (op == Clock) {
        val = m_p.cur_clock();
        return true;
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
void Controller<Pulser>::dump_dds(int i)
{
    string_ostream stm;
    m_p.dump_dds(stm, i);
    auto str = stm.get_buf();
    char *start = &str[0];
    while (*start) {
        char *p = strchrnul(start, '\n');
        auto end = !*p;
        *p = 0;
        Log::info("%s\n", start);
        if (end)
            break;
        start = p + 1;
    }
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
            Log::info("DDS %d initialized\n", i);
        if (force) {
            dump_dds(i);
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
bool Controller<Pulser>::has_ttl_ovr()
{
    return m_p.ttl_lomask() || m_p.ttl_himask();
}

template<typename Pulser>
template<bool checked>
std::pair<uint32_t,bool> Controller<Pulser>::run_cmd(const ReqCmd *cmd, Runner *runner)
{
    switch (cmd->opcode) {
    case TTL: {
        // Should have been caught by concurrent_get/set.
        assert(!cmd->has_res && !cmd->is_override);
        if (cmd->operand) {
            m_ttl = m_ttl | cmd->val;
        }
        else {
            m_ttl = m_ttl & ~cmd->val;
        }
        if (runner)
            runner->update_preserve_ttl(m_ttl);
        m_p.template ttl<checked>(m_ttl, Seq::PulseTime::Min);
        return {Seq::PulseTime::Min, false};
    }
    case DDSFreq: {
        bool is_override = cmd->is_override;
        bool has_res = cmd->has_res;
        int chn = cmd->operand;
        uint32_t val = cmd->val;
        assert(chn < 22);
        auto &ovr = m_dds_ovr[chn];
        // If override is on, treat all set command as override command.
        if (!is_override && ovr.freq != uint32_t(-1) && !has_res)
            is_override = true;
        if (is_override) {
            // Should be handled by the cache in ctrl_iface.
            assert(!has_res);
            if (val == ovr.freq)
                return {0, false};
            ovr.freq = val;
            if (val == uint32_t(-1)) {
                return {0, false};
            }
            else {
                m_p.template dds_set_freq<checked>(chn, val);
                return {Seq::PulseTime::DDSFreq, false};
            }
        }
        if (!has_res) {
            m_p.template dds_set_freq<checked>(chn, val);
            return {Seq::PulseTime::DDSFreq, false};
        }
        m_p.template dds_get_freq<checked>(chn);
        return {Seq::PulseTime::DDSFreq, true};
    }
    case DDSAmp: {
        bool is_override = cmd->is_override;
        bool has_res = cmd->has_res;
        int chn = cmd->operand;
        uint32_t val = cmd->val;
        uint16_t val16 = uint16_t(val);
        assert(chn < 22);
        auto &ovr = m_dds_ovr[chn];
        // If override is on, treat all set command as override command.
        if (!is_override && ovr.amp_enable && !has_res)
            is_override = true;
        if (is_override) {
            // Should be handled by the cache in ctrl_iface.
            assert(!has_res);
            // val16 == ovr.amp does not imply the override is on
            // so we need to check that separately.
            if (val16 == ovr.amp && ovr.amp_enable) {
                return {0, false};
            }
            else if (val == uint32_t(-1)) {
                if (ovr.amp_enable)
                    ovr.amp_enable = false;
                return {0, false};
            }
            else {
                ovr.amp = uint16_t(val16 & ((1 << 12) - 1));
                ovr.amp_enable = true;
                m_p.template dds_set_amp<checked>(chn, val16);
                return {Seq::PulseTime::DDSAmp, false};
            }
        }
        if (!has_res) {
            m_p.template dds_set_amp<checked>(chn, val16);
            return {Seq::PulseTime::DDSAmp, false};
        }
        m_p.template dds_get_amp<checked>(chn);
        return {Seq::PulseTime::DDSAmp, true};
    }
    case DDSPhase: {
        bool is_override = cmd->is_override;
        bool has_res = cmd->has_res;
        int chn = cmd->operand;
        uint32_t val = cmd->val;
        uint16_t val16 = uint16_t(val);
        assert(chn < 22);
        auto &ovr = m_dds_ovr[chn];
        // If override is on, treat all set command as override command.
        if (!is_override && ovr.phase_enable && !has_res)
            is_override = true;
        if (is_override) {
            // Should be handled by the cache in ctrl_iface.
            assert(!has_res);
            // val16 == ovr.phase does not imply the override is on
            // so we need to check that separately.
            if (val16 == ovr.phase && ovr.phase_enable) {
                return {0, false};
            }
            else if (val == uint32_t(-1)) {
                if (ovr.phase_enable)
                    ovr.phase_enable = false;
                return {0, false};
            }
            else {
                ovr.phase = val16;
                ovr.phase_enable = true;
                m_dds_phase[chn] = val16;
                m_p.template dds_set_phase<checked>(chn, val16);
                return {Seq::PulseTime::DDSPhase, false};
            }
        }
        if (!has_res) {
            m_dds_phase[chn] = val16;
            m_p.template dds_set_phase<checked>(chn, val16);
            return {Seq::PulseTime::DDSPhase, false};
        }
        m_p.template dds_get_phase<checked>(chn);
        return {Seq::PulseTime::DDSPhase, true};
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
        return {Seq::PulseTime::Clock, false};
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
    sync_ttl();
    m_p.set_hold();
    // `toggle_init` is needed to clear the force release flag
    // so that `set_hold` can work.
    m_p.toggle_init();
    seq->state.store(SeqStart, std::memory_order_relaxed);
    backend_event();

    Runner runner(*this, seq->ttl_mask, seq->seq_len_ns);
    try {
        if (unlikely(seq->is_cmd)) {
            Seq::CmdList::ExeState exestate;
            exestate.run(runner, seq->code, seq->code_len);
        }
        else {
            Seq::ByteCode::ExeState exestate;
            exestate.run(runner, seq->code, seq->code_len);
        }
    }
    catch (const std::exception &err) {
        Log::error("Error while running sequence: %s.\n", err.what());
    }
    // Stop the timing check with a short wait.
    // Do this before releasing the hold since the effect of the time check flag
    // in the previous instruction last until the next one.
    runner.template wait<false>(Seq::PulseTime::Min);
    m_p.release_hold();
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
    runner.enable_process_cmd();
    if (!seq->is_cmd) {
        // 10ms
        runner.template wait<false>(1000000);
        runner.template clock<false>(255);
    }
    if (!m_p.timing_ok())
        Log::warn("Timing failures: %u cycles.\n", m_p.underflow_cycle());
    m_p.clear_error();

    // Doing this check before this sequence will make the current sequence
    // more likely to work. However, that increase the latency and the DDS
    // reset only happen very infrequently so let's do it after the sequence
    // for better efficiency.
    for (int i = 0; i < NDDS; i++) {
        if (m_dds_exist[i].load(std::memory_order_relaxed) && check_dds(i)) {
            Log::info("DDS %d reinit\n", i);
            dump_dds(i);
        }
    }
}

template<typename Pulser>
void Controller<Pulser>::worker()
{
    while (wait(500000000)) { // Wake up every 500ms
        if (auto seq = get_seq()) {
            if (seq->cancel.load(std::memory_order_relaxed)) {
                seq->state.store(SeqCancel, std::memory_order_relaxed);
            }
            else {
                run_seq(seq);
            }
            finish_seq();
        }
        if (m_p.is_finished())
            sync_ttl();
        detect_dds();
        process_reqcmd<false>();
        detect_dds();
    }
}

} // anonymous namespace

namespace Molecube {

NACS_EXPORT() std::unique_ptr<CtrlIFace> CtrlIFace::create(bool dummy)
{
    if (!dummy) {
        if (auto addr = Molecube::Pulser::address())
            return std::unique_ptr<CtrlIFace>(new Controller<Pulser>(Pulser(addr)));
        Log::warn("Failed to create real pulser, use dummy pulser instead.\n");
    }
    return std::unique_ptr<CtrlIFace>(new Controller<DummyPulser>(DummyPulser()));
}

}
