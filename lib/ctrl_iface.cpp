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

#include <nacs-utils/fd_utils.h>
#include <nacs-utils/timer.h>

#include <chrono>

namespace Molecube {

void CtrlIFace::CmdCache::set(ReqOP op, uint32_t operand, bool is_override, uint32_t val)
{
    auto t = getTime();
    auto update_entry = [&] (auto &entry) {
        entry.t = t;
        entry.val = val;
        for (auto &cb: entry.cbs)
            cb(val);
        entry.cbs.clear();
    };
    auto key = cache_key(op, operand, is_override);
    auto &entry = m_cache[key];
    update_entry(entry);
    // If this is a normal set command for DDS, we need to update the override value as well.
    if (!is_override && (op == DDSFreq || op == DDSAmp || op == DDSPhase)) {
        key = cache_key(op, operand, true);
        auto it = m_cache.find(key);
        if (it != m_cache.end() && it->second.val != uint32_t(-1)) {
            update_entry(it->second);
        }
    }
}

bool CtrlIFace::CmdCache::get(ReqOP op, uint32_t operand, bool is_override, callback_t cb)
{
    auto key = cache_key(op, operand, is_override);
    auto t = getTime();
    auto &entry = m_cache[key];
    if (t - entry.t <= 100000000) {
        // < 0.1s
        cb(entry.val);
        return true;
    }
    // DDS overrides are only kept in software so no need to ask the backend.
    if (is_override) {
        // Initially off (-1) by default.
        if (entry.t == 0)
            entry.val = -1;
        cb(entry.val);
        return true;
    }
    auto was_empty = entry.cbs.empty();
    entry.cbs.push_back(std::move(cb));
    return !was_empty;
}

inline bool CtrlIFace::CmdCache::has_dds_ovr()
{
    for (int i = 0; i < 22; i++) {
        for (auto op: (ReqOP[]){DDSFreq, DDSAmp, DDSPhase}) {
            auto key = cache_key(op, i, true);
            auto it = m_cache.find(key);
            if (it == m_cache.end() || it->second.val == uint32_t(-1))
                continue;
            return true;
        }
    }
    return false;
}

CtrlIFace::CtrlIFace()
    : m_bkend_evt(openEvent(0, EFD_NONBLOCK | EFD_CLOEXEC))
{
}

bool CtrlIFace::wait(int64_t maxt)
{
    std::unique_lock<std::mutex> lk(m_ftend_lck);
    auto pred = [&] {
        return m_quit || m_seq_queue.get_filter() || m_cmd_queue.get_filter();
    };
    if (maxt < 0) {
        m_ftend_evt.wait(lk, pred);
    }
    else {
        m_ftend_evt.wait_for(lk, std::chrono::nanoseconds(maxt), pred);
    }
    return !m_quit;
}

auto CtrlIFace::get_seq() -> ReqSeq*
{
    return m_seq_queue.get_filter();
}

void CtrlIFace::finish_seq()
{
    m_seq_queue.forward_filter();
    backend_event();
}

auto CtrlIFace::get_cmd() -> ReqCmd*
{
    return m_cmd_queue.get_filter();
}

void CtrlIFace::finish_cmd()
{
    m_cmd_queue.forward_filter();
}

NACS_EXPORT() uint64_t CtrlIFace::_run_code(bool is_cmd, uint64_t seq_len_ns,
                                            uint32_t ttl_mask,
                                            const uint8_t *code, size_t code_len,
                                            std::unique_ptr<ReqSeqNotify> notify,
                                            AnyPtr storage)
{
    set_dirty();
    auto id = ++m_seq_cnt;
    auto seq = m_seq_alloc.alloc(id, seq_len_ns, code, code_len, ttl_mask, is_cmd,
                                 std::move(notify), std::move(storage));
    {
        std::lock_guard<std::mutex> lk(m_ftend_lck);
        m_seq_queue.push(seq);
    }
    m_ftend_evt.notify_all();
    return id;
}

NACS_EXPORT() bool CtrlIFace::cancel_seq(uint64_t id)
{
    bool found = false;
    for (auto seq: m_seq_queue) {
        if (id == 0 || seq->id == id) {
            found = true;
            seq->cancel.store(true, std::memory_order_relaxed);
        }
    }
    return found;
}

void CtrlIFace::backend_event()
{
    writeEvent(m_bkend_evt);
}

void CtrlIFace::send_cmd(const ReqCmd &_cmd)
{
    auto cmd = m_cmd_alloc.alloc(_cmd);
    {
        std::lock_guard<std::mutex> lk(m_ftend_lck);
        m_cmd_queue.push(cmd);
    }
    m_ftend_evt.notify_all();
}

void CtrlIFace::send_set_cmd(ReqOP op, uint32_t operand, bool is_override, uint32_t val)
{
    set_dirty();
    if (!concurrent_set(op, operand, is_override, val))
        send_cmd(ReqCmd{uint8_t(op & 0xf), 0, uint8_t(is_override),
                        operand & ((1 << 26) - 1), val});
    m_cmd_cache.set(op, operand, is_override, val);
}

void CtrlIFace::send_get_cmd(ReqOP op, uint32_t operand, bool is_override, callback_t cb)
{
    set_observed();
    uint32_t val = 0;
    if (concurrent_get(op, operand, is_override, val)) {
        m_cmd_cache.set(op, operand, is_override, val);
        cb(val);
        return;
    }
    if (m_cmd_cache.get(op, operand, is_override, std::move(cb)))
        return;
    send_cmd(ReqCmd{uint8_t(op & 0xf), 1, uint8_t(is_override),
                    operand & ((1 << 26) - 1), 0});
}

// TTL channels are get and set in batch so they don't really fit
// the cache used for other channels.
// Since the implementation of `Controller` always returns ttl get request concurrently
// we'll just skip the cache and callback storage for now.
void CtrlIFace::send_ttl_set_cmd(uint32_t operand, bool is_override, uint32_t val)
{
    set_dirty();
    if (!concurrent_set(TTL, operand, is_override, val)) {
        send_cmd(ReqCmd{TTL, 0, uint8_t(is_override), operand & ((1 << 26) - 1), val});
    }
}

void CtrlIFace::send_ttl_get_cmd(uint32_t operand, bool is_override, callback_t cb)
{
    set_observed();
    // Unsupported for now
    // We could support asynchronous get by adding a TTL specific callback queue.
    uint32_t val = 0;
    if (!concurrent_get(TTL, operand, is_override, val))
        abort();
    cb(val);
}

NACS_EXPORT() void CtrlIFace::set_ttl(uint32_t mask, bool val)
{
    if (!mask)
        return;
    send_ttl_set_cmd(uint32_t(val), false, mask);
}

NACS_EXPORT() void CtrlIFace::set_ttl_ovr(uint32_t mask, int val)
{
    if (!mask)
        return;
    send_ttl_set_cmd(uint32_t(val), true, mask);
}

NACS_EXPORT() void CtrlIFace::get_ttl(callback_t cb)
{
    send_ttl_get_cmd(0, false, std::move(cb));
}

NACS_EXPORT() void CtrlIFace::get_ttl_ovrlo(callback_t cb)
{
    send_ttl_get_cmd(0, true, std::move(cb));
}

NACS_EXPORT() void CtrlIFace::get_ttl_ovrhi(callback_t cb)
{
    send_ttl_get_cmd(1, true, std::move(cb));
}

NACS_EXPORT() void CtrlIFace::set_dds(ReqOP op, int chn, uint32_t val)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    send_set_cmd(op, chn, false, val);
}

NACS_EXPORT() void CtrlIFace::set_dds_ovr(ReqOP op, int chn, uint32_t val)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    send_set_cmd(op, chn, true, val);
}

NACS_EXPORT() void CtrlIFace::get_dds(ReqOP op, int chn, callback_t cb)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    send_get_cmd(op, chn, false, std::move(cb));
}

NACS_EXPORT() void CtrlIFace::get_dds_ovr(ReqOP op, int chn, callback_t cb)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    send_get_cmd(op, chn, true, std::move(cb));
}

NACS_EXPORT() void CtrlIFace::reset_dds(int chn)
{
    set_dirty();
    send_cmd(ReqCmd{DDSReset, 0, 0, uint32_t(chn & ((1 << 26) - 1)), 0});
    // Clear override
    m_cmd_cache.set(DDSFreq, chn, true, -1);
    m_cmd_cache.set(DDSAmp, chn, true, -1);
    m_cmd_cache.set(DDSPhase, chn, true, -1);
}

NACS_EXPORT() void CtrlIFace::set_clock(uint8_t val)
{
    send_set_cmd(Clock, 0, false, val);
}

NACS_EXPORT() void CtrlIFace::get_clock(callback_t cb)
{
    send_get_cmd(Clock, 0, false, std::move(cb));
}

NACS_EXPORT() void CtrlIFace::quit()
{
    {
        // The lock here is overkill. However, the wait already needs a lock and we
        // don't care about the performance here so just use the lock here...
        std::lock_guard<std::mutex> lk(m_ftend_lck);
        m_quit = true;
    }
    m_ftend_evt.notify_all();
}

NACS_EXPORT() void CtrlIFace::run_frontend()
{
    readEvent(m_bkend_evt);
    auto run_callbacks = [&] (auto seq) {
        auto state = seq->state.load(std::memory_order_relaxed);
        auto pstate = seq->processed_state;
        if (state >= SeqStart && pstate < SeqStart)
            seq->notify->start(seq->id);
        if (state >= SeqFlushed && pstate < SeqFlushed)
            seq->notify->flushed(seq->id);
        if (state >= SeqEnd && pstate < SeqEnd)
            seq->notify->end(seq->id);
        if (state == SeqCancel && pstate != SeqCancel)
            seq->notify->cancel(seq->id);
        if (pstate != state) {
            seq->processed_state = state;
        }
    };
    // Get the current sequence first so that we know everything before
    // it will be popped below.
    // This guarantees that the callbacks will be executed in order without leaving a gap.
    // If we get the current sequence after popping all the finished ones, the
    // current sequence may not be the once immediately after the last finished one we
    // process if a sequence finished in between.
    auto curseq = m_seq_queue.peek_filter();
    while (auto seq = m_seq_queue.pop()) {
        if (curseq && curseq == seq)
            curseq = nullptr;
        run_callbacks(seq);
        m_seq_alloc.free(seq);
    }
    if (curseq)
        run_callbacks(curseq);
    while (auto cmd = m_cmd_queue.pop()) {
        if (cmd->has_res)
            m_cmd_cache.set(ReqOP(cmd->opcode), cmd->operand,
                            cmd->is_override, cmd->val);
        m_cmd_alloc.free(cmd);
    }
}

inline void CtrlIFace::set_dirty()
{
    // If no one has looked at our state, we don't need to invalidate their cache.
    // Also, if the last one who looked at the state ID still think there's a sequence running
    // there's no need to explicitly invalidate things either.
    if (m_observed && !m_had_seq) {
        m_dirty = true;
    }
}

inline void CtrlIFace::set_observed()
{
    m_observed = true;
}

NACS_EXPORT() uint64_t CtrlIFace::get_state_id()
{
    bool has_seq = m_seq_queue.peek().second;
    bool new_id = false;
    if (has_seq != m_had_seq) {
        // If we started or stopped running sequences since the last query,
        // always increase the state id.
        m_had_seq = has_seq;
        ++m_state_cnt;
        new_id = true;
    }
    else if (m_dirty) {
        // And if the state changed since the last time someone looked, increase the id.
        ++m_state_cnt;
        new_id = true;
    }
    m_dirty = false;
    // We've created a new state ID which has never been observed yet.
    if (new_id)
        m_observed = false;
    return (uint64_t(has_seq) << 63) | m_state_cnt;
}

NACS_EXPORT() std::pair<bool,bool> CtrlIFace::has_pending()
{
    auto cmdres = m_cmd_queue.peek();
    if (cmdres.second)
        return {true, true};
    auto seqres = m_seq_queue.peek();
    if (seqres.second)
        return {true, true};
    return {seqres.first || cmdres.first, false};
}

NACS_EXPORT() bool CtrlIFace::has_dds_ovr()
{
    return m_cmd_cache.has_dds_ovr();
}

}
