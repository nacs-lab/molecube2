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

namespace Molecube {

void CtrlIFace::CmdCache::set(ReqOP op, uint32_t operand, bool is_override, uint32_t val)
{
    auto key = cache_key(op, operand, is_override);
    auto t = getTime();
    auto &entry = m_cache[key];
    entry.t = t;
    entry.val = val;
    for (auto &cb: entry.cbs)
        cb(val);
    entry.cbs.clear();
}

bool CtrlIFace::CmdCache::get(ReqOP op, uint32_t operand, bool is_override,
                              std::function<void(uint32_t)> cb)
{
    auto key = cache_key(op, operand, is_override);
    auto t = getTime();
    auto &entry = m_cache[key];
    if (t - entry.t <= 500000000) {
        // < 0.5s
        cb(entry.val);
        return true;
    }
    auto was_empty = entry.cbs.empty();
    entry.cbs.push_back(std::move(cb));
    return !was_empty;
}

CtrlIFace::CtrlIFace()
    : m_bkend_evt(openEvent(0, EFD_NONBLOCK | EFD_CLOEXEC))
{
}

void CtrlIFace::wait()
{
    std::unique_lock<std::mutex> lk(m_ftend_lck);
    m_ftend_evt.wait(lk, [&] {
            return m_seq_queue.get_filter() || m_cmd_queue.get_filter();
        });
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

void CtrlIFace::backend_event()
{
    writeEvent(m_bkend_evt);
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
        if (pstate != state) {
            seq->processed_state = state;
        }
    };
    while (auto seq = m_seq_queue.pop()) {
        run_callbacks(seq);
        m_seq_alloc.free(seq);
    }
    if (auto seq = m_seq_queue.peak().first) {
        run_callbacks(seq);
    }
}

}
