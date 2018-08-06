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

namespace Molecube {

CtrlIFace::CtrlIFace()
    : m_bkend_evt(openEvent(0, EFD_NONBLOCK | EFD_CLOEXEC))
{
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

NACS_EXPORT() uint64_t CtrlIFace::_run_code(bool is_cmd, uint64_t seq_len_ns,
                                            uint32_t ttl_mask,
                                            const uint8_t *code, size_t code_len,
                                            std::unique_ptr<ReqSeqNotify> notify,
                                            AnyPtr storage)
{
    auto id = ++m_seq_cnt;
    auto seq = m_seq_alloc.alloc(id, seq_len_ns, code, code_len, ttl_mask, is_cmd,
                                 std::move(notify), std::move(storage));
    m_seq_queue.push(seq);
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
