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

#include <nacs-seq/zynq/bytecode.h>
#include <nacs-seq/zynq/cmdlist.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <tuple>

namespace {
using namespace Molecube;

// The DMA reads the instructions in blocks of 16 x 64bits.
static constexpr size_t dma_block_sz = 16 * 8;
// Each buffer is read with a single DMA command, which can read at most 1024 blocks.
static constexpr size_t dma_max_buff_sz = 64 * 1024;
static_assert(dma_max_buff_sz / dma_block_sz <= 1024);
// Minimum number of buffers needed to run DMA sequences.
static constexpr size_t dma_min_buffs = 3;

// Set the DDS write timing on the pulser.
// A negative value keeps the current value of that timing parameter.
template<typename Pulser>
static void set_pulser_dds_timing1(Pulser &p, int adsu, int wrlow, int adhd,
                                   int fuddl, int fudhd)
{
    std::array<int,5> timings{adsu, wrlow, adhd, fuddl, fudhd};
    bool has_set = false;
    bool has_default = false;
    for (auto t: timings) {
        if (t < 0) {
            has_default = true;
            continue;
        }
        if (t > 7)
            throw std::runtime_error("DDS write timing out of bound (max is 7).");
        has_set = true;
    }
    if (!has_set)
        return;
    if (auto hw_ver = p.hw_version(); !hw_ver.check_at_least({5, 4}))
        throw std::runtime_error("DDS write timing requires hardware version 5.4, got " + to_string(hw_ver));
    if (has_default) {
        auto def_timings = p.get_dds_timing1();
        for (int i = 0; i < 5; i++) {
            if (timings[i] < 0) {
                timings[i] = (int)def_timings[i];
            }
        }
    }
    p.set_dds_timing1((uint32_t)timings[0], (uint32_t)timings[1], (uint32_t)timings[2],
                      (uint32_t)timings[3], (uint32_t)timings[4]);
}

template<typename Pulser>
class Controller final : public CtrlIFace {
    Controller(const Controller&) = delete;
    void operator=(const Controller&) = delete;

public:
    Controller(Pulser &&p);
    ~Controller();

private:
    class Runner;

    void run_frontend() override;
    void set_ttl(int bank, uint32_t mask, bool val) override;
    void set_ttl_ovr(int bank, uint32_t mask, int val) override;
    uint32_t get_ttl(int bank) override;
    TTLOvr get_ttl_ovr(int bank) override;
    void set_dds(ReqOP op, int chn, uint32_t val) override;
    void set_dds_ovr(ReqOP op, int chn, uint32_t val) override;
    void get_dds(ReqOP op, int chn, callback_t cb) override;
    void get_dds_ovr(ReqOP op, int chn, callback_t cb) override;
    void reset_dds(int chn) override;
    void set_clock(uint8_t val) override;
    uint8_t get_clock() override;

    // Pending callbacks for the DDS parameter `op` of channel `chn`
    // waiting for the result from the backend.
    std::vector<callback_t> &dds_get_cbs(ReqOP op, int chn)
    {
        return m_dds_get_cbs[chn][op - DDSFreq];
    }
    // Update the cached value of a DDS parameter and run the pending callbacks.
    void update_dds_cache(ReqOP op, int chn, uint32_t val);

    std::vector<int> get_active_dds() override;
    bool has_ttl_ovr() override;

    bool check_dds(int chn);
    void detect_dds(bool force=false);
    void dump_dds(int i);
    void set_dds_timing1(int adsu, int wrlow, int adhd, int fuddl, int fudhd) override;

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
        for (int i = 0; i < NUM_TTL_BANKS; i++) {
            auto ttl = m_p.cur_ttl(i);
            if (ttl != m_ttl[i]) {
                Log::warn("TTL out of sync: has %04x, actual %04x\n", m_ttl[i], ttl);
                m_ttl[i] = ttl;
            }
        }
    }

    Pulser m_p;
    uint32_t m_ttl[NUM_TTL_BANKS];
    uint16_t m_dds_phase[NDDS] = {0};
    // Reinitialize is a complicated sequence and is rarely needed
    // so only do that after the sequence finishes.
    bool m_dds_pending_reset[NDDS] = {false};
    std::atomic<bool> m_dds_exist[NDDS] = {};
    uint64_t m_dds_check_time = 0;
    ReqCmd *m_cmd_waiting = nullptr;

    std::vector<callback_t> m_dds_get_cbs[NDDS][3];

    // Must be after all the members used by the worker thread.
    std::thread m_worker;
};

template<typename Pulser>
class Controller<Pulser>::Runner {
public:
    Runner(Controller &ctrl, const std::array<uint32_t,NUM_TTL_BANKS> &ttlmask,
           uint64_t seq_len_ns)
        : m_ctrl(ctrl),
          m_ttlmask(ttlmask),
          m_process_cmd(seq_len_ns > 1000000000ul) // 1s
    {
        for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
            m_preserve_ttl[bank] = (~ttlmask[bank]) & ctrl.m_ttl[bank];
        }
    }
    void ttl1(int full_chn, bool val, uint64_t t)
    {
        auto chn = uint8_t(full_chn & 31);
        int bank = full_chn / 32;
        ttl(setBit(m_ctrl.m_ttl[bank], chn, val), t, bank);
    }
    void ttl(uint32_t ttl, uint64_t t, int bank)
    {
        m_ctrl.m_ttl[bank] = ttl | m_preserve_ttl[bank];
        if (t <= 1000) {
            // 10us
            m_t += t;
            m_ctrl.m_p.template ttl<true>(m_ctrl.m_ttl[bank], (uint32_t)t, bank);
        }
        else {
            m_t += 100;
            m_ctrl.m_p.template ttl<true>(m_ctrl.m_ttl[bank], 100, bank);
            wait(t - 100);
        }
    }
    void dds_freq(uint8_t chn, uint32_t freq)
    {
        if (unlikely(m_ctrl.dds_overridden(DDSFreq, chn))) {
            wait(Seq::Zynq::PulseTime::DDSFreq);
            return;
        }
        m_t += Seq::Zynq::PulseTime::DDSFreq;
        m_ctrl.m_p.template dds_set_freq<true>(chn, freq);
    }
    void dds_amp(uint8_t chn, uint16_t amp)
    {
        if (unlikely(m_ctrl.dds_overridden(DDSAmp, chn))) {
            wait(Seq::Zynq::PulseTime::DDSAmp);
            return;
        }
        m_t += Seq::Zynq::PulseTime::DDSAmp;
        m_ctrl.m_p.template dds_set_amp<true>(chn, amp);
    }
    void dds_phase(uint8_t chn, uint16_t phase)
    {
        if (unlikely(m_ctrl.dds_overridden(DDSPhase, chn))) {
            wait(Seq::Zynq::PulseTime::DDSPhase);
            return;
        }
        m_ctrl.m_dds_phase[chn] = phase;
        m_t += Seq::Zynq::PulseTime::DDSPhase;
        m_ctrl.m_p.template dds_set_phase<true>(chn, phase);
    }
    void dds_detphase(uint8_t chn, uint16_t detphase)
    {
        if (unlikely(m_ctrl.dds_overridden(DDSPhase, chn))) {
            wait(Seq::Zynq::PulseTime::DDSPhase);
            return;
        }
        dds_phase(chn, uint16_t(m_ctrl.m_dds_phase[chn] + detphase));
    }
    void dac(uint8_t chn, uint16_t V)
    {
        m_t += Seq::Zynq::PulseTime::DAC;
        m_ctrl.m_p.template dac<true>(chn, V);
    }
    template<bool checked=true>
    void clock(uint8_t period)
    {
        m_t += Seq::Zynq::PulseTime::Clock;
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
    void wait_trigger(uint8_t chn, bool trig_raise, uint32_t timeout)
    {
        m_ctrl.m_p.template wait_trigger<true>(chn, trig_raise, timeout);
        // Reset start time since the sequence will actually proceed when we
        // received a trigger from this command.
        m_t = 0;
        m_start_t = getCoarseTime();
    }
    void update_preserve_ttl(uint32_t ttl, int bank)
    {
        m_preserve_ttl[bank] = ttl & ~m_ttlmask[bank];
    }
    void enable_process_cmd()
    {
        m_process_cmd = true;
    }

private:
    Controller &m_ctrl;
    const std::array<uint32_t,NUM_TTL_BANKS> m_ttlmask;
    std::array<uint32_t,NUM_TTL_BANKS> m_preserve_ttl;
    uint64_t m_t{0};

    uint64_t m_start_t{getCoarseTime()};
    // Minimum time we stay ahead of the sequence.
    const uint64_t m_min_t{max(getCoarseRes() * 20, 500000000)}; // 0.5s
    bool m_process_cmd;

    bool m_released = false;
};

template<typename Pulser>
Controller<Pulser>::Controller(Pulser &&p)
    : m_p(std::move(p)),
      m_worker(&Controller<Pulser>::worker, this)
{
    for (int i = 0; i < NUM_TTL_BANKS; i++)
        m_ttl[i] = m_p.cur_ttl(i);
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
void Controller<Pulser>::update_dds_cache(ReqOP op, int chn, uint32_t val)
{
    auto &cache = dds_cache(op, chn);
    cache.cached = true;
    cache.val = val;
    cache.t = getTime();
    auto &cbs = dds_get_cbs(op, chn);
    for (auto &cb: cbs)
        cb(val);
    cbs.clear();
}

template<typename Pulser>
void Controller<Pulser>::set_ttl(int bank, uint32_t mask, bool val)
{
    if (!mask)
        return;
    set_dirty();
    send_cmd(TTL, false, uint32_t(val) | uint32_t(bank << 2), mask);
}

template<typename Pulser>
void Controller<Pulser>::set_ttl_ovr(int bank, uint32_t mask, int val)
{
    if (!mask)
        return;
    set_dirty();
    // TTL overrides are set concurrently without sending a command in the queue
    // since they don't need to be synchronized.
    assert(0 <= bank && bank < NUM_TTL_BANKS);
    auto lomask = m_p.ttl_lomask(bank);
    auto himask = m_p.ttl_himask(bank);
    if (val == 0) {
        m_p.set_ttl_lomask((lomask | mask), bank);
        m_p.set_ttl_himask((himask & ~mask), bank);
    }
    else if (val == 1) {
        m_p.set_ttl_lomask((lomask & ~mask), bank);
        m_p.set_ttl_himask((himask | mask), bank);
    }
    else {
        m_p.set_ttl_lomask((lomask & ~mask), bank);
        m_p.set_ttl_himask((himask & ~mask), bank);
    }
}

template<typename Pulser>
uint32_t Controller<Pulser>::get_ttl(int bank)
{
    set_observed();
    assert(0 <= bank && bank < NUM_TTL_BANKS);
    return (m_p.cur_ttl(bank) | m_p.ttl_himask(bank)) & ~m_p.ttl_lomask(bank);
}

template<typename Pulser>
auto Controller<Pulser>::get_ttl_ovr(int bank) -> TTLOvr
{
    set_observed();
    assert(0 <= bank && bank < NUM_TTL_BANKS);
    return {m_p.ttl_lomask(bank), m_p.ttl_himask(bank)};
}

template<typename Pulser>
void Controller<Pulser>::set_dds(ReqOP op, int chn, uint32_t val)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    set_dirty();
    send_cmd(op, false, chn, val);
    // A normal set of an overridden parameter is treated as an override by the backend.
    update_dds_cache(op, chn, val);
}

template<typename Pulser>
void Controller<Pulser>::set_dds_ovr(ReqOP op, int chn, uint32_t val)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    set_dirty();
    auto &cache = dds_cache(op, chn);
    if (val == uint32_t(-1)) {
        // Turning off the override doesn't change the value in the hardware.
        cache.overridden.store(false, std::memory_order_relaxed);
        return;
    }
    send_cmd(op, false, chn, val);
    cache.overridden.store(true, std::memory_order_relaxed);
    update_dds_cache(op, chn, val);
}

template<typename Pulser>
void Controller<Pulser>::get_dds(ReqOP op, int chn, callback_t cb)
{
    set_observed();
    auto &cache = dds_cache(op, chn);
    if (cache.cached && getTime() - cache.t <= 100000000) {
        // < 0.1s
        cb(cache.val);
        return;
    }
    // Ask the backend for the current value.
    // Only send the query if there isn't one in flight already.
    auto &cbs = dds_get_cbs(op, chn);
    auto was_empty = cbs.empty();
    cbs.push_back(std::move(cb));
    if (was_empty) {
        send_cmd(op, true, chn, 0);
    }
}

template<typename Pulser>
void Controller<Pulser>::get_dds_ovr(ReqOP op, int chn, callback_t cb)
{
    // DDS overrides are only kept in software so no need to ask the backend.
    set_observed();
    auto &cache = dds_cache(op, chn);
    cb(cache.overridden.load(std::memory_order_relaxed) ? cache.val : uint32_t(-1));
}

template<typename Pulser>
void Controller<Pulser>::reset_dds(int chn)
{
    set_dirty();
    send_cmd(DDSReset, false, uint32_t(chn), 0);
    // Clear override and the cached values that are reset.
    for (auto &param: m_dds_cache[chn].params)
        param.reset();
}

template<typename Pulser>
void Controller<Pulser>::set_clock(uint8_t val)
{
    set_dirty();
    send_cmd(Clock, false, 0, val);
}

template<typename Pulser>
uint8_t Controller<Pulser>::get_clock()
{
    set_observed();
    return m_p.cur_clock();
}

template<typename Pulser>
void Controller<Pulser>::run_frontend()
{
    run_seq_frontend();
    while (auto cmd = pop_cmd()) {
        if (cmd->has_res) {
            // Only the DDS parameters are queried from the backend.
            auto op = ReqOP(cmd->opcode);
            assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
            update_dds_cache(op, cmd->operand, cmd->val);
        }
        free_cmd(cmd);
    }
}

template<typename Pulser>
void Controller<Pulser>::set_dds_timing1(int adsu, int wrlow, int adhd,
                                         int fuddl, int fudhd)
{
    set_pulser_dds_timing1(m_p, adsu, wrlow, adhd, fuddl, fudhd);
}

template<typename Pulser>
bool Controller<Pulser>::check_dds(int chn)
{
    assert(!m_cmd_waiting);
    // The overrides are cleared by the frontend (`reset_dds`).
    if (m_dds_pending_reset[chn]) {
        m_dds_phase[chn] = 0;
    }
    auto res = m_p.check_dds(chn, m_dds_pending_reset[chn]);
    m_dds_pending_reset[chn] = false;
    return res;
}

template<typename Pulser>
void Controller<Pulser>::dump_dds(int i)
{
    assert(!m_cmd_waiting);
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
    assert(!m_cmd_waiting);
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
    for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
        if (m_p.ttl_lomask(bank) || m_p.ttl_himask(bank)) {
            return true;
        }
    }
    return false;
}

template<typename Pulser>
template<bool checked>
std::pair<uint32_t,bool> Controller<Pulser>::run_cmd(const ReqCmd *cmd, Runner *runner)
{
    switch (cmd->opcode) {
    case TTL: {
        // Should have been caught by set_ttl_ovr/get_ttl*.
        assert(!cmd->has_res);
        auto type = cmd->operand & 3;
        auto bank = cmd->operand >> 2;
        if (type) {
            m_ttl[bank] = m_ttl[bank] | cmd->val;
        }
        else {
            m_ttl[bank] = m_ttl[bank] & ~cmd->val;
        }
        if (runner)
            runner->update_preserve_ttl(m_ttl[bank], bank);
        m_p.template ttl<checked>(m_ttl[bank], Seq::Zynq::PulseTime::Min, bank);
        return {Seq::Zynq::PulseTime::Min, false};
    }
    // The overrides are handled by the frontend so both the normal and the override
    // set commands only need to write the value to the DDS.
    case DDSFreq: {
        int chn = cmd->operand;
        assert(chn < NDDS);
        if (!cmd->has_res) {
            m_p.template dds_set_freq<checked>(chn, cmd->val);
            return {Seq::Zynq::PulseTime::DDSFreq, false};
        }
        m_p.template dds_get_freq<checked>(chn);
        return {Seq::Zynq::PulseTime::DDSFreq, true};
    }
    case DDSAmp: {
        int chn = cmd->operand;
        assert(chn < NDDS);
        if (!cmd->has_res) {
            m_p.template dds_set_amp<checked>(chn, uint16_t(cmd->val));
            return {Seq::Zynq::PulseTime::DDSAmp, false};
        }
        m_p.template dds_get_amp<checked>(chn);
        return {Seq::Zynq::PulseTime::DDSAmp, true};
    }
    case DDSPhase: {
        int chn = cmd->operand;
        assert(chn < NDDS);
        if (!cmd->has_res) {
            auto val16 = uint16_t(cmd->val);
            m_dds_phase[chn] = val16;
            m_p.template dds_set_phase<checked>(chn, val16);
            return {Seq::Zynq::PulseTime::DDSPhase, false};
        }
        m_p.template dds_get_phase<checked>(chn);
        return {Seq::Zynq::PulseTime::DDSPhase, true};
    }
    case DDSReset: {
        assert(!cmd->has_res && cmd->val == 0);
        int chn = cmd->operand;
        assert(chn < 22);
        m_dds_pending_reset[chn] = true;
        return {0, false};
    }
    case Clock:
        assert(!cmd->has_res && cmd->operand == 0);
        m_p.template clock<checked>(uint8_t(cmd->val));
        return {Seq::Zynq::PulseTime::Clock, false};
    default:
        return {0, false};
    }
}

template<typename Pulser>
template<bool checked>
std::pair<bool,bool> Controller<Pulser>::try_get_result()
{
    if (m_cmd_waiting) {
        if (!m_p.try_get_result(m_cmd_waiting->val))
            return {true, false};
        m_cmd_waiting = nullptr;
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
    // Already has a command waiting for result.
    // We need to wait for it to finished before being able to process the next one.
    if (m_cmd_waiting)
        return {0, true};
    if (auto cmd = get_cmd()) {
        auto res = run_cmd<checked>(cmd, runner);
        if (res.second) {
            m_cmd_waiting = cmd;
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
        auto ver = seq->ver;
        assert(ver == 1 || ver == 2 || ver == 3);
        if (unlikely(seq->is_cmd)) {
            Seq::Zynq::CmdList::ExeState exestate;
            if (ver > 1)
                exestate.min_time = Seq::Zynq::PulseTime::Min2;
            exestate.run(runner, seq->code.data(), seq->code.size(), ver);
        }
        else {
            Seq::Zynq::ByteCode::ExeState exestate;
            if (ver > 1)
                exestate.min_time = Seq::Zynq::PulseTime::Min2;
            exestate.run(runner, seq->code.data(), seq->code.size(), ver);
        }
    }
    catch (const std::exception &err) {
        Log::error("Error while running sequence: %s.\n", err.what());
    }
    // Stop the timing check with a short wait.
    // Do this before releasing the hold since the effect of the time check flag
    // in the previous instruction last until the next one.
    runner.template wait<false>(Seq::Zynq::PulseTime::Min);
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
        Log::warn("Timing failures.\n");
    m_p.clear_error();

    if (!m_cmd_waiting) {
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
        if (!m_cmd_waiting) {
            detect_dds();
        }
        process_reqcmd<false>();
        if (!m_cmd_waiting) {
            detect_dds();
        }
    }
}

struct DMABuff {
    uint8_t *virt;
    uintptr_t phy;
    size_t size;
};

template<typename Pulser>
static void free_dma_buffs(std::vector<DMABuff> &buffs)
{
    for (auto &buff: buffs)
        Pulser::free_buffer(buff.virt, buff.size);
    buffs.clear();
}

// Allocate as many DMA buffers as possible, starting from the largest size.
// Returns an empty list if there are not enough buffers to run DMA sequences.
template<typename Pulser>
static std::vector<DMABuff> alloc_dma_buffs()
{
    std::vector<DMABuff> buffs;
    for (size_t size = dma_max_buff_sz; size >= 4096;) {
        auto buff = Pulser::alloc_buffer(size);
        if (!buff) {
            size /= 2;
            continue;
        }
        auto phy = Pulser::buffer_addr(buff);
        // The DMA address must be page aligned and cannot cross a 1MB boundary.
        assert((phy & 0xfff) == 0 && (phy >> 20) == ((phy + size - 1) >> 20));
        buffs.push_back({(uint8_t*)buff, phy, size});
    }
    if (buffs.size() < dma_min_buffs)
        free_dma_buffs<Pulser>(buffs);
    return buffs;
}

template<typename Pulser>
class ControllerDMA final : public CtrlIFace {
    ControllerDMA(const ControllerDMA&) = delete;
    void operator=(const ControllerDMA&) = delete;

public:
    // Takes the ownership of the DMA buffers allocated with `alloc_dma_buffs`.
    ControllerDMA(Pulser &&p, std::vector<DMABuff> &&dma_buffs);
    ~ControllerDMA();

private:
    void run_frontend() override;
    void set_ttl(int bank, uint32_t mask, bool val) override;
    void set_ttl_ovr(int bank, uint32_t mask, int val) override;
    uint32_t get_ttl(int bank) override;
    TTLOvr get_ttl_ovr(int bank) override;
    void set_dds(ReqOP op, int chn, uint32_t val) override;
    void set_dds_ovr(ReqOP op, int chn, uint32_t val) override;
    void get_dds(ReqOP op, int chn, callback_t cb) override;
    void get_dds_ovr(ReqOP op, int chn, callback_t cb) override;
    void reset_dds(int chn) override;
    void set_clock(uint8_t val) override;
    uint8_t get_clock() override;

    std::vector<int> get_active_dds() override;
    bool has_ttl_ovr() override;

    void set_dds_timing1(int adsu, int wrlow, int adhd, int fuddl, int fudhd) override;
    DDSInstTiming get_dds_inst_timing() const override;

    Pulser m_p;
    std::vector<DMABuff> m_dma_buffs;
};

template<typename Pulser>
ControllerDMA<Pulser>::ControllerDMA(Pulser &&p, std::vector<DMABuff> &&dma_buffs)
    : m_p(std::move(p)),
      m_dma_buffs(std::move(dma_buffs))
{
    assert(m_dma_buffs.size() >= dma_min_buffs);
}

template<typename Pulser>
ControllerDMA<Pulser>::~ControllerDMA()
{
    quit();
    free_dma_buffs<Pulser>(m_dma_buffs);
}

// TTL channels are get and set directly in the hardware
// without going through the command queue.
template<typename Pulser>
void ControllerDMA<Pulser>::set_ttl(int bank, uint32_t mask, bool val)
{
    if (!mask)
        return;
    set_dirty();
    assert(0 <= bank && bank < NUM_TTL_BANKS);
    // Set the channels one byte at a time.
    for (int i = 0; i < 4; i++) {
        auto byte = uint8_t(mask >> (i * 8));
        if (!byte)
            continue;
        m_p.set_ttl(bank * 4 + i, val ? 0 : byte, val ? byte : 0);
    }
}

template<typename Pulser>
void ControllerDMA<Pulser>::set_ttl_ovr(int bank, uint32_t mask, int val)
{
    if (!mask)
        return;
    set_dirty();
    // TTL overrides are set concurrently without sending a command in the queue
    // since they don't need to be synchronized.
    assert(0 <= bank && bank < NUM_TTL_BANKS);
    auto lomask = m_p.ttl_lomask(bank);
    auto himask = m_p.ttl_himask(bank);
    if (val == 0) {
        m_p.set_ttl_lomask((lomask | mask), bank);
        m_p.set_ttl_himask((himask & ~mask), bank);
    }
    else if (val == 1) {
        m_p.set_ttl_lomask((lomask & ~mask), bank);
        m_p.set_ttl_himask((himask | mask), bank);
    }
    else {
        m_p.set_ttl_lomask((lomask & ~mask), bank);
        m_p.set_ttl_himask((himask & ~mask), bank);
    }
}

template<typename Pulser>
uint32_t ControllerDMA<Pulser>::get_ttl(int bank)
{
    set_observed();
    assert(0 <= bank && bank < NUM_TTL_BANKS);
    return (m_p.cur_ttl(bank) | m_p.ttl_himask(bank)) & ~m_p.ttl_lomask(bank);
}

template<typename Pulser>
auto ControllerDMA<Pulser>::get_ttl_ovr(int bank) -> TTLOvr
{
    set_observed();
    assert(0 <= bank && bank < NUM_TTL_BANKS);
    return {m_p.ttl_lomask(bank), m_p.ttl_himask(bank)};
}

template<typename Pulser>
void ControllerDMA<Pulser>::set_dds(ReqOP op, int chn, uint32_t val)
{
}

template<typename Pulser>
void ControllerDMA<Pulser>::set_dds_ovr(ReqOP op, int chn, uint32_t val)
{
}

template<typename Pulser>
void ControllerDMA<Pulser>::get_dds(ReqOP op, int chn, callback_t cb)
{
}

template<typename Pulser>
void ControllerDMA<Pulser>::get_dds_ovr(ReqOP op, int chn, callback_t cb)
{
}

template<typename Pulser>
void ControllerDMA<Pulser>::reset_dds(int chn)
{
}

template<typename Pulser>
void ControllerDMA<Pulser>::set_clock(uint8_t val)
{
    set_dirty();
    m_p.set_clock(val);
}

template<typename Pulser>
uint8_t ControllerDMA<Pulser>::get_clock()
{
    set_observed();
    return m_p.cur_clock();
}

template<typename Pulser>
void ControllerDMA<Pulser>::run_frontend()
{
}

template<typename Pulser>
void ControllerDMA<Pulser>::set_dds_timing1(int adsu, int wrlow, int adhd,
                                            int fuddl, int fudhd)
{
    set_pulser_dds_timing1(m_p, adsu, wrlow, adhd, fuddl, fudhd);
}

template<typename Pulser>
DDSInstTiming ControllerDMA<Pulser>::get_dds_inst_timing() const
{
    return DDSInstTiming::get(m_p);
}

template<typename Pulser>
std::vector<int> ControllerDMA<Pulser>::get_active_dds()
{
    return {};
}

template<typename Pulser>
bool ControllerDMA<Pulser>::has_ttl_ovr()
{
    for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
        if (m_p.ttl_lomask(bank) || m_p.ttl_himask(bank)) {
            return true;
        }
    }
    return false;
}

// Create the DMA controller if the DMA mode is enabled and can be used,
// otherwise create the normal controller.
// Throws if the DMA mode is required but cannot be used.
template<typename Pulser>
static std::unique_ptr<CtrlIFace> create_controller(Pulser &&p,
                                                    Config::DMAEnable dma_enable)
{
    if (!p.support_dma()) {
        if (dma_enable == Config::DMAEnable::Required)
            throw std::runtime_error("DMA mode required but not supported.\n");
        return std::unique_ptr<CtrlIFace>(new Controller<Pulser>(std::move(p)));
    }
    if (dma_enable != Config::DMAEnable::Disabled) {
        auto buffs = alloc_dma_buffs<Pulser>();
        if (!buffs.empty())
            return std::unique_ptr<CtrlIFace>(new ControllerDMA<Pulser>(std::move(p),
                                                                        std::move(buffs)));
        if (dma_enable == Config::DMAEnable::Required) {
            throw std::runtime_error("DMA mode required but failed to allocate DMA buffers.\n");
        }
    }
    // Make sure the DMA mode is disabled in the hardware if we are not using it.
    p.set_dma_control(0);
    return std::unique_ptr<CtrlIFace>(new Controller<Pulser>(std::move(p)));
}

} // anonymous namespace

namespace Molecube {

NACS_EXPORT() std::unique_ptr<CtrlIFace> CtrlIFace::create(bool dummy,
                                                           Config::DMAEnable dma_enable)
{
    if (!dummy) {
        if (auto addr = Molecube::Pulser::address())
            return create_controller(Pulser(addr), dma_enable);
        throw std::runtime_error("Failed to create real pulser, use dummy pulser instead.\n");
    }
    return create_controller(DummyPulser(), dma_enable);
}

}
