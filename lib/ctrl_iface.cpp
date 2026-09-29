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

NACS_EXPORT() uint64_t CtrlIFace::_run_code(SeqType type, uint32_t ver, uint64_t seq_len_ns,
                                            const std::array<uint32_t,NUM_TTL_BANKS> &ttl_mask,
                                            std::span<const uint8_t> code,
                                            std::unique_ptr<ReqSeqNotify> notify,
                                            AnyPtr storage)
{
    set_dirty();
    auto id = ++m_seq_cnt;
    notify->set_id(id);
    auto seq = m_seq_alloc.alloc(id, seq_len_ns, code, ttl_mask, ver, type,
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

void CtrlIFace::send_cmd(ReqOP op, bool has_res, uint32_t operand, uint32_t val)
{
    auto cmd = m_cmd_alloc.alloc(ReqCmd{uint8_t(op & 0xf), uint8_t(has_res),
                                        operand & ((1 << 26) - 1), val});
    {
        std::lock_guard<std::mutex> lk(m_ftend_lck);
        m_cmd_queue.push(cmd);
    }
    m_ftend_evt.notify_all();
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

void CtrlIFace::run_seq_frontend()
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
}

void CtrlIFace::set_dirty()
{
    // If no one has looked at our state, we don't need to invalidate their cache.
    // Also, if the last one who looked at the state ID still think there's a sequence running
    // there's no need to explicitly invalidate things either.
    if (m_observed && !m_had_seq) {
        m_dirty = true;
    }
}

void CtrlIFace::set_observed()
{
    m_observed = true;
}

NACS_EXPORT() uint64_t CtrlIFace::get_state_id()
{
    auto [seq, done] = m_seq_queue.peek();
    bool has_seq = seq && !done;
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
    for (auto &cache: m_dds_cache) {
        for (auto &param: cache.params) {
            if (param.overridden.load(std::memory_order_relaxed)) {
                return true;
            }
        }
    }
    return false;
}

}
