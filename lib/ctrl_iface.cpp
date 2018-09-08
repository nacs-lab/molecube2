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
    // DDS overrides are only kept in software so no need to ask the backend.
    if (is_override && op != TTL) {
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

CtrlIFace::CtrlIFace()
    : m_bkend_evt(openEvent(0, EFD_NONBLOCK | EFD_CLOEXEC))
{
}

bool CtrlIFace::wait()
{
    std::unique_lock<std::mutex> lk(m_ftend_lck);
    m_ftend_evt.wait(lk, [&] {
            return m_quit || m_seq_queue.get_filter() || m_cmd_queue.get_filter();
        });
    return m_quit;
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

NACS_PROTECTED() uint64_t CtrlIFace::_run_code(bool is_cmd, uint64_t seq_len_ns,
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
    if (!concurrent_set(op, operand, is_override, val))
        send_cmd(ReqCmd{uint8_t(op & 0xf), 0, uint8_t(is_override),
                    operand & ((1 << 26) - 1), val});
    m_cmd_cache.set(op, operand, is_override, val);
}

void CtrlIFace::send_get_cmd(ReqOP op, uint32_t operand, bool is_override,
                             std::function<void(uint32_t)> cb)
{
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

NACS_PROTECTED() void CtrlIFace::set_ttl(int chn, bool val)
{
    send_set_cmd(TTL, uint32_t(chn), false, val);
}

NACS_PROTECTED() void CtrlIFace::set_ttl_all(uint32_t val)
{
    send_set_cmd(TTL, uint32_t(-1), false, val);
}

NACS_PROTECTED() void CtrlIFace::set_ttl_ovrhi(uint32_t val)
{
    send_set_cmd(TTL, 1, true, val);
}

NACS_PROTECTED() void CtrlIFace::set_ttl_ovrlo(uint32_t val)
{
    send_set_cmd(TTL, 0, true, val);
}

NACS_PROTECTED() void CtrlIFace::get_ttl(std::function<void(uint32_t)> cb)
{
    send_get_cmd(TTL, 0, false, std::move(cb));
}

NACS_PROTECTED() void CtrlIFace::get_ttl_ovrhi(std::function<void(uint32_t)> cb)
{
    send_get_cmd(TTL, 1, true, std::move(cb));
}

NACS_PROTECTED() void CtrlIFace::get_ttl_ovrlo(std::function<void(uint32_t)> cb)
{
    send_get_cmd(TTL, 0, true, std::move(cb));
}

NACS_PROTECTED() void CtrlIFace::set_dds(ReqOP op, int chn, uint32_t val)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    send_set_cmd(op, chn, false, val);
}

NACS_PROTECTED() void CtrlIFace::set_dds_ovr(ReqOP op, int chn, uint32_t val)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    send_set_cmd(op, chn, true, val);
}

NACS_PROTECTED() void CtrlIFace::get_dds(ReqOP op, int chn, std::function<void(uint32_t)> cb)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    send_get_cmd(op, chn, false, std::move(cb));
}

NACS_PROTECTED() void CtrlIFace::get_dds_ovr(ReqOP op, int chn, std::function<void(uint32_t)> cb)
{
    assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
    send_get_cmd(op, chn, true, std::move(cb));
}

NACS_PROTECTED() void CtrlIFace::reset_dds(int chn)
{
    send_cmd(ReqCmd{DDSReset, 0, 0, uint32_t(chn & ((1 << 26) - 1)), 0});
    // Clear overwrite
    m_cmd_cache.set(DDSFreq, chn, true, -1);
    m_cmd_cache.set(DDSAmp, chn, true, -1);
    m_cmd_cache.set(DDSPhase, chn, true, -1);
}

NACS_PROTECTED() void CtrlIFace::set_clock(uint32_t val)
{
    send_set_cmd(Clock, 0, false, val);
}

NACS_PROTECTED() void CtrlIFace::get_clock(std::function<void(uint32_t)> cb)
{
    send_get_cmd(Clock, 0, false, std::move(cb));
}

NACS_PROTECTED() void CtrlIFace::quit()
{
    {
        // The lock here is overkill. However, the wait already needs a lock and we
        // don't care about the performance here so just use the lock here...
        std::lock_guard<std::mutex> lk(m_ftend_lck);
        m_quit = true;
    }
    m_ftend_evt.notify_all();
}

NACS_PROTECTED() void CtrlIFace::run_frontend()
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
    // Get the current sequence first so that we know everything before
    // it will be popped below and it is guaranteed that newly finished sequences
    // won't have the callback executed after the ones for the current sequence
    // in case it finishes right after we tried popping it here.
    auto curseq = m_seq_queue.peak().first;
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

}
