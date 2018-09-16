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

void Server::send_header(zmq::message_t &addr)
{
    m_zmqsock.send(addr, ZMQ_SNDMORE);
    m_zmqsock.send(m_empty, ZMQ_SNDMORE);
}

void Server::send_reply(zmq::message_t &addr, zmq::message_t &msg)
{
    send_header(addr);
    m_zmqsock.send(msg);
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

}
