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

#ifndef LIBMOLECUBE_SERVER_H
#define LIBMOLECUBE_SERVER_H

#include "ctrl_iface.h"
#include "namesconfig.h"

#include <nacs-utils/zmq_utils.h>

#include <atomic>

namespace Molecube {

using namespace NaCs;

class Config;

class Server {
public:
    Server(const Config &conf);
    void run();
    void stop()
    {
        m_running.store(false, std::memory_order_relaxed);
    }

private:
    struct SeqStatus {
        struct Wait {
            uint8_t what;
            std::vector<zmq::message_t> addr;
        };
        uint64_t id;
        std::vector<Wait> wait{};
        bool flushed{false};
    };

    void send_reply(std::vector<zmq::message_t> &addr, zmq::message_t &msg);
    void send_reply(std::vector<zmq::message_t> &addr, zmq::message_t &&msg)
    {
        send_reply(addr, msg);
    }
    bool recv_more(zmq::message_t &msg);
    void process_zmq();
    // Read the sequence id from the message.
    // Check if the message is 16+suffix bytes and if the second 8 bytes matches the server id.
    // Return 0 if the check fails. Otherwise, return the 64bit int from the first 8 bytes.
    uint64_t get_seq_id(zmq::message_t &msg, size_t suffix=0);
    bool process_set_dds(zmq::message_t &msg, bool is_ovr);
    bool process_run_seq(std::vector<zmq::message_t> &addr, bool is_cmd);
    SeqStatus *find_seqstatus(uint64_t id);
    bool process_set_names(zmq::message_t &msg, NamesConfig &names);
    void process_get_names(std::vector<zmq::message_t> &addr, NamesConfig &names);
    void ensure_runtime_dir();
    void run_startup();
    void process_set_startup(std::vector<zmq::message_t> &addr, zmq::message_t &msg);

    template<typename CB>
    void get_override_dds(CB cb);

    const Config &m_conf;
    const uint64_t m_id;
    std::unique_ptr<CtrlIFace> m_ctrl;
    zmq::context_t m_zmqctx;
    zmq::socket_t m_zmqsock;
    zmq::pollitem_t m_zmqpoll[2];
    zmq::message_t m_empty{0};
    volatile std::atomic_bool m_running{false};
    std::vector<SeqStatus> m_seq_status{};
    uint64_t m_name_id = 0;
    NamesConfig m_ttl_names;
    NamesConfig m_dds_names;
};

}

#endif // LIBMOLECUBE_SERVER_H
