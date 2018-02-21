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

auto CtrlIFace::pop_seq() -> ReqSeq*
{
    ReqSeq *res;
    {
        std::lock_guard<std::mutex> locker(m_seq_lock);
        if (m_seq_reqs.empty())
            return nullptr;
        res = m_seq_reqs.back();
        m_seq_reqs.pop_back();
    }
    backend_event();
    return res;
}

void CtrlIFace::reply_seq(ReqSeq *seq)
{
    {
        std::lock_guard<std::mutex> locker(m_seq_lock);
        m_seq_replies.push_back(seq);
    }
    backend_event();
}

void CtrlIFace::backend_event()
{
    writeEvent(m_bkend_evt);
}

void CtrlIFace::clear_backend_event()
{
    readEvent(m_bkend_evt);
}

}
