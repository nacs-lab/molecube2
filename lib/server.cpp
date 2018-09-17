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

#include "server.h"
#include "config.h"

#include <time.h>

namespace Molecube {

namespace {

// Return start time in ms as server ID.
// This is obviously not guaranteed to be unique across multiple servers
// but is good enough to detect server restart.
static uint64_t get_server_id()
{
    timespec t;
    clock_gettime(CLOCK_REALTIME_COARSE, &t);
    return uint64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

}

inline void Server::send_header(zmq::message_t &addr)
{
    m_zmqsock.send(addr, ZMQ_SNDMORE);
    m_zmqsock.send(m_empty, ZMQ_SNDMORE);
}

inline void Server::send_reply(zmq::message_t &addr, zmq::message_t &msg)
{
    send_header(addr);
    m_zmqsock.send(msg);
}

inline bool Server::recv_more(zmq::message_t &msg)
{
    return ZMQ::recv_more(m_zmqsock, msg);
}

inline uint64_t Server::get_seq_id(zmq::message_t &msg)
{
    if (msg.size() != 16)
        return 0;
    uint64_t id;
    memcpy(&id, (char*)msg.data() + 8, 8);
    if (id != m_id)
        return 0;
    memcpy(&id, msg.data(), 8);
    return id;
}

Server::Server(const Config &conf)
    : m_conf(conf),
      m_id(get_server_id()),
      m_ctrl(CtrlIFace::create(conf.dummy)),
      m_zmqctx(),
      m_zmqsock(m_zmqctx, ZMQ_ROUTER),
      m_zmqpoll{{(void*)m_zmqsock, 0, ZMQ_POLLIN, 0},
    {nullptr, m_ctrl->backend_fd(), ZMQ_POLLIN, 0}}
{
    m_zmqsock.bind(m_conf.listen);
}

void Server::run()
{
    m_running.store(true, std::memory_order_relaxed);
    while (m_running.load(std::memory_order_relaxed)) {
        // TODO set timeout based on controller state
        zmq::poll(m_zmqpoll, sizeof(m_zmqpoll) / sizeof(zmq::pollitem_t), -1);
        if (m_zmqpoll[1].revents & ZMQ_POLLIN) {
            m_ctrl->run_frontend();
        }
        if (m_zmqpoll[0].revents & ZMQ_POLLIN) {
            process_zmq();
        }
    }
}

uint8_t Server::process_set_dds(zmq::message_t &msg, bool is_ovr)
{
    size_t msgsz = msg.size();
    if (msgsz % 5 != 0)
        return 1;
    uint8_t *data = (uint8_t*)msg.data();
    // First check if all the channels are valid
    for (size_t i = 0; i < msgsz; i += 5) {
        auto chn = data[i];
        if ((chn >> 6) >= 3 || (chn & 0x3f) >= 22) {
            return 1;
        }
    }
    static constexpr CtrlIFace::ReqOP ops[3] = {CtrlIFace::DDSFreq, CtrlIFace::DDSAmp,
                                                CtrlIFace::DDSPhase};
    for (size_t i = 0; i < msgsz; i += 5) {
        auto chn = data[i];
        auto op = ops[chn >> 6];
        chn = chn & 0x3f;
        uint32_t val;
        memcpy(&val, &data[i + 1], 4);
        if (is_ovr) {
            m_ctrl->set_dds_ovr(op, chn, val);
        }
        else {
            m_ctrl->set_dds(op, chn, val);
        }
    }
    return 0;
}

void Server::process_zmq()
{
    zmq::message_t addr;
    m_zmqsock.recv(&addr);

    zmq::message_t msg;
    m_zmqsock.recv(&msg);
    assert(msg.size() == 0);

    if (!recv_more(msg)) {
        // Empty request?
        send_reply(addr, ZMQ::bits_msg(false));
        return;
    }
    if (ZMQ::match(msg, "cancel_seq")) {
        bool res;
        if (!recv_more(msg)) {
            res = m_ctrl->cancel_seq(0);
        }
        else if (uint64_t id = get_seq_id(msg)) {
            res = m_ctrl->cancel_seq(id);
        }
        else {
            res = false;
        }
        send_reply(addr, ZMQ::bits_msg(res));
    }
    else if (ZMQ::match(msg, "state_id")) {
        std::array<uint64_t,2> id{m_ctrl->get_state_id(), m_id};
        send_reply(addr, ZMQ::bits_msg(id));
    }
    else if (ZMQ::match(msg, "override_ttl")) {
        if (!recv_more(msg) || msg.size() != 12) {
            send_reply(addr, ZMQ::bits_msg<uint8_t>(1));
            goto out;
        }
        uint32_t masks[3];
        memcpy(masks, msg.data(), 12);
        for (int i = 0; i < 3; i++) {
            if (masks[i]) {
                m_ctrl->set_ttl(masks[i], i);
            }
        }
        // get_ttl_ovr* returns immediately and we only have two functions to call
        // so it's just as fast to do the two calls in series
        // and it's also easier to implement this way.
        m_ctrl->get_ttl_ovrlo([addr{std::move(addr)}, this] (uint32_t lo) mutable {
                m_ctrl->get_ttl_ovrhi([addr{std::move(addr)}, lo, this] (uint32_t hi) mutable {
                        std::array<uint32_t,2> masks{lo, hi};
                        send_reply(addr, ZMQ::bits_msg(masks));
                    });
            });
    }
    else if (ZMQ::match(msg, "set_ttl")) {
        if (!recv_more(msg) || msg.size() != 8) {
            send_reply(addr, ZMQ::bits_msg<uint8_t>(1));
            goto out;
        }
        uint32_t masks[2];
        memcpy(masks, msg.data(), 8);
        if (masks[0])
            m_ctrl->set_ttl(masks[0], false);
        if (masks[1])
            m_ctrl->set_ttl(masks[1], true);
        m_ctrl->get_ttl([addr{std::move(addr)}, masks, this] (uint32_t v) mutable {
                // The get can arrive faster than the set so manually mask the
                // value to avoid confusion.
                v = (v & ~masks[0]) | masks[1];
                send_reply(addr, ZMQ::bits_msg(v));
            });
    }
    else if (ZMQ::match(msg, "override_dds")) {
        send_reply(addr, ZMQ::bits_msg(recv_more(msg) && process_set_dds(msg, true)));
    }
    else if (ZMQ::match(msg, "set_dds")) {
        send_reply(addr, ZMQ::bits_msg(recv_more(msg) && process_set_dds(msg, false)));
    }
    else if (ZMQ::match(msg, "reset_dds")) {
        if (!recv_more(msg) || msg.size() != 1) {
            send_reply(addr, ZMQ::bits_msg<uint8_t>(1));
            goto out;
        }
        int chn = *(char*)msg.data();
        if (chn >= 22) {
            send_reply(addr, ZMQ::bits_msg<uint8_t>(1));
            goto out;
        }
        m_ctrl->reset_dds(chn);
        send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
    }
    else if (ZMQ::match(msg, "set_clock")) {
        if (!recv_more(msg) || msg.size() != 1) {
            send_reply(addr, ZMQ::bits_msg<uint8_t>(1));
            goto out;
        }
        uint8_t clock = *(uint8_t*)msg.data();
        m_ctrl->set_clock(clock);
        send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
    }
    else if (ZMQ::match(msg, "get_clock")) {
        m_ctrl->get_clock([addr{std::move(addr)}, this] (uint32_t v) mutable {
                send_reply(addr, ZMQ::bits_msg(uint8_t(v)));
            });
    }
out:
    ZMQ::readall(m_zmqsock);
}

}
