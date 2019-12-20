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

#include <nacs-utils/errors.h>
#include <nacs-utils/log.h>
#include <nacs-utils/streams.h>
#include <nacs-utils/timer.h>

#include <nacs-seq/cmdlist.h>

#include <chrono>
#include <fstream>
#include <thread>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

static constexpr CtrlIFace::ReqOP dds_ops[3] = {CtrlIFace::DDSFreq, CtrlIFace::DDSAmp,
                                                CtrlIFace::DDSPhase};

static void push_dds_res(std::vector<uint8_t> &res, uint8_t chn, uint32_t v)
{
    auto oldn = res.size();
    res.resize(oldn + 5);
    res[oldn] = chn;
    memcpy(&res[oldn + 1], &v, 4);
}

static void free_malloc_msg(void *data, void*)
{
    free(data);
}

}

#define _NACS_EXPORT NACS_EXPORT()

inline void Server::send_reply(std::vector<zmq::message_t> &addr, zmq::message_t &msg)
{
    ZMQ::send_addr(m_zmqsock, addr, m_empty);
    ZMQ::send(m_zmqsock, msg);
}

inline bool Server::recv_more(zmq::message_t &msg)
{
    return ZMQ::recv_more(m_zmqsock, msg);
}

inline uint64_t Server::get_seq_id(zmq::message_t &msg, size_t suffix)
{
    if (msg.size() != 16 + suffix)
        return 0;
    uint64_t id;
    memcpy(&id, (char*)msg.data() + 8, 8);
    if (id != m_id)
        return 0;
    memcpy(&id, msg.data(), 8);
    return id;
}

void Server::ensure_runtime_dir()
{
    struct stat info;
    if (stat(m_conf.runtime_dir.c_str(), &info) != 0) {
        auto dir = m_conf.runtime_dir;
        char *start = &dir[0];
        char *p = start;
        while (true) {
            while (*p == '/')
                p++;
            if (!*p)
                break;
            while (*p && *p != '/')
                p++;
            bool end = *p == 0;
            *p = 0;
            mkdir(start, 0755);
            if (end)
                break;
            *p = '/';
        }
    }
    else if ((info.st_mode & S_IFDIR) == 0) {
        Log::error("Runtime directory `%s` exists but is not a directory.\n",
                   m_conf.runtime_dir.c_str());
        return;
    }
}

void Server::run_startup()
{
    std::ifstream istm(m_conf.runtime_dir + "/startup.cmdbin");
    if (!istm.good()) {
        Log::info("No startup sequence found.\n");
        return;
    }
    std::string str(std::istreambuf_iterator<char>(istm), {});
    if (str.size() < 16) {
        Log::error("`startup.cmdbin` too short.\n");
        return;
    }
    auto str_data = (const uint8_t*)str.data();
    auto str_sz = str.size();

    uint32_t ver;
    memcpy(&ver, str_data, 4);
    str_data += 4;
    str_sz -= 4;
    if (ver != 1) {
        Log::error("Wrong startup file version.\n");
        return;
    }

    uint64_t len_ns;
    memcpy(&len_ns, str_data, 8);
    str_data += 8;
    str_sz -= 8;

    uint32_t ttl_mask;
    memcpy(&ttl_mask, str_data, 4);
    str_data += 4;
    str_sz -= 4;

    bool finished = false;
    struct Notify: CtrlIFace::ReqSeqNotify {
        Notify(bool *finished)
            : finished(finished)
        {
        }
        ~Notify() override
        {
            *finished = true;
        }
        bool *finished;
    };
    m_ctrl->run_code(true, len_ns, ttl_mask, str_data, str_sz,
                     std::make_unique<Notify>(&finished));
    while (!finished) {
        using namespace std::literals;
        m_ctrl->run_frontend();
        std::this_thread::sleep_for(1ms);
    }
    Log::info("Startup sequence finished.\n");
}

_NACS_EXPORT Server::Server(const Config &conf)
    : m_conf(conf),
      m_id(get_server_id()),
      m_ctrl(CtrlIFace::create(conf.dummy)),
      m_zmqctx(),
      m_zmqsock(m_zmqctx, ZMQ_ROUTER),
      m_zmqpoll{{(void*)m_zmqsock, 0, ZMQ_POLLIN, 0},
                {nullptr, m_ctrl->backend_fd(), ZMQ_POLLIN, 0}},
      m_ttl_names(conf.runtime_dir + "/ttl.yaml"),
      m_dds_names(conf.runtime_dir + "/dds.yaml")
{
    Log::info("Listening on: `%s`\n", m_conf.listen.c_str());
    m_zmqsock.bind(m_conf.listen);
    // This will come after we try to use the directory above
    // This shouldn't cause any major issue though since if the directory didn't exist,
    // the file in it won't exist either and the loading will fail no matter what.
    ensure_runtime_dir();
    if (m_ttl_names.get().size() != 32) {
        m_ttl_names.get().resize(32);
        m_ttl_names.save();
    }
    if (m_dds_names.get().size() != 22) {
        m_dds_names.get().resize(22);
        m_dds_names.save();
    }
    run_startup();
}

_NACS_EXPORT void Server::run()
{
    m_running.store(true, std::memory_order_relaxed);
    while (m_running.load(std::memory_order_relaxed)) {
        auto has_pending = m_ctrl->has_pending();
        long timeout = 1000;
        if (has_pending.second) {
            timeout = 0;
        }
        else if (has_pending.first) {
            timeout = 10;
        }
        zmq::poll(m_zmqpoll, sizeof(m_zmqpoll) / sizeof(zmq::pollitem_t), timeout);
        m_ctrl->run_frontend();
        if (m_zmqpoll[0].revents & ZMQ_POLLIN) {
            process_zmq();
        }
    }
}

bool Server::process_set_dds(zmq::message_t &msg, bool is_ovr)
{
    size_t msgsz = msg.size();
    if (msgsz % 5 != 0)
        return false;
    uint8_t *data = (uint8_t*)msg.data();
    // First check if all the channels are valid
    for (size_t i = 0; i < msgsz; i += 5) {
        auto chn = data[i];
        // Channel id out of bound.
        if ((chn >> 6) >= 3 || (chn & 0x3f) >= 22)
            return false;
        if (is_ovr)
            continue;
        uint32_t val;
        memcpy(&val, &data[i + 1], 4);
        // channel value out of bound
        if (val == uint32_t(-1)) {
            return false;
        }
    }
    for (size_t i = 0; i < msgsz; i += 5) {
        auto chn = data[i];
        auto op = dds_ops[chn >> 6];
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
    return true;
}

bool Server::process_run_seq(std::vector<zmq::message_t> &addr, bool is_cmd)
{
    zmq::message_t msg;
    // No version
    if (!recv_more(msg) || msg.size() != 4)
        return false;
    uint32_t ver;
    memcpy(&ver, msg.data(), 4);
    if (ver != 1)
        return false;
    // Not long enough
    if (!recv_more(msg) || msg.size() < 12)
        return false;
    auto msg_data = (const uint8_t*)msg.data();
    auto msg_sz = msg.size();

    Timer timer;
    Log::info("Running %s: %zu bytes.\n", is_cmd ? "command list" : "sequence", msg_sz);

    uint64_t len_ns;
    memcpy(&len_ns, msg_data, 8);
    msg_data += 8;
    msg_sz -= 8;

    uint32_t ttl_mask;
    memcpy(&ttl_mask, msg_data, 4);
    msg_data += 4;
    msg_sz -= 4;

    struct Notify: CtrlIFace::ReqSeqNotify {
        void start(uint64_t _id) override
        {
            (void)_id;
            assert(id == _id);
            Log::info("Start time: %.1f ms\n", (double)timer.elapsed() / 1000000.0);
            send_reply();
        }
        void flushed(uint64_t _id) override
        {
            (void)_id;
            assert(id == _id);
            send_reply();
            auto status = server.find_seqstatus(id);
            assert(status);
            status->flushed = true;
            for (auto &wait: status->wait) {
                if (wait.what == 0) {
                    server.send_reply(wait.addr, ZMQ::bits_msg<uint8_t>(0));
                }
            }
        }
        void end(uint64_t _id) override
        {
            (void)_id;
            assert(id == _id);
            send_reply();
            Log::info("Finish time: %.1f ms\n", (double)timer.elapsed() / 1000000.0);
            finalize(false);
        }
        void cancel(uint64_t _id) override
        {
            (void)_id;
            assert(id == _id);
            send_reply();
            finalize(true);
        }
        Notify(Server &server, std::vector<zmq::message_t> addr, Timer timer)
            : server(server),
              addr(std::move(addr)),
              timer(std::move(timer))
        {
        }
        ~Notify() override
        {
            send_reply();
            finalize(true);
        }
        void send_reply()
        {
            if (replied)
                return;
            replied = true;
            std::array<uint8_t,18> res;
            memcpy(&res[0], &id, 8);
            memcpy(&res[8], &server.m_id, 8);
            res[16] = server.m_ctrl->has_ttl_ovr();
            res[17] = server.m_ctrl->has_dds_ovr();
            server.send_reply(addr, ZMQ::bits_msg(res));
        }
        void finalize(bool cancel)
        {
            auto status = server.find_seqstatus(id);
            if (!status)
                return;
            for (auto &wait: status->wait) {
                if (wait.what == 0 && status->flushed)
                    continue;
                server.send_reply(wait.addr, ZMQ::bits_msg<uint8_t>(cancel));
            }
            auto idx = status - &server.m_seq_status[0];
            server.m_seq_status.erase(server.m_seq_status.begin() + idx);
        }
        Server &server;
        std::vector<zmq::message_t> addr;
        Timer timer;
        bool replied = false;
        uint64_t id = -1;
    };

    auto notify = new Notify(*this, std::move(addr), std::move(timer));
    auto id = m_ctrl->run_code(is_cmd, len_ns, ttl_mask, msg_data, msg_sz,
                               std::unique_ptr<CtrlIFace::ReqSeqNotify>(notify),
                               std::move(msg));
    notify->id = id;
    m_seq_status.push_back(SeqStatus{id});
    Log::info("Sequence %llu scheduled.\n", (unsigned long long)id);
    return true;
}

auto Server::find_seqstatus(uint64_t id) -> SeqStatus*
{
    for (auto &status: m_seq_status) {
        if (status.id == id) {
            return &status;
        }
    }
    return nullptr;
}

bool Server::process_set_names(zmq::message_t &msg, NamesConfig &names)
{
    bool has_set = false;
    auto &vec = names.get();
    auto nchn = vec.size();

    char *p = (char*)msg.data();
    size_t sz = msg.size();
    char *end = p + sz;
    while (p + 1 < end) {
        uint8_t chn = (uint8_t)*p;
        p++;
        auto n = strnlen(p, end - p);
        if (n == size_t(end - p)) {
            Log::warn("Name not NUL terminated\n");
            break;
        }
        if (chn >= nchn) {
            Log::warn("Channel %u out of range: [0, %u)\n", chn, (unsigned)nchn);
        }
        else {
            has_set = true;
            vec[chn] = std::string(p, n);
        }
        p += n + 1;
    }
    if (has_set)
        names.save();

    return has_set;
}

void Server::process_get_names(std::vector<zmq::message_t> &addr, NamesConfig &names)
{
    malloc_ostream ostm;
    auto &vec = names.get();
    for (size_t i = 0; i < vec.size(); i++) {
        auto &str = vec[i];
        if (str.empty())
            continue;
        uint8_t chn = (uint8_t)i;
        ostm.write((const char*)&chn, 1);
        ostm.write(&str[0], str.size() + 1);
    }
    size_t msgsz;
    auto ptr = ostm.get_buf(msgsz);
    send_reply(addr, zmq::message_t(ptr, msgsz, free_malloc_msg));
}

void Server::process_set_startup(std::vector<zmq::message_t> &addr, zmq::message_t &msg)
{
    Log::info("Setting startup file.\n");
    char *data = (char*)msg.data();
    size_t size = strnlen(data, msg.size());
    if (size == msg.size()) {
        Log::error("Startup sequence not NUL terminated.\n");
        send_reply(addr, ZMQ::bits_msg<uint8_t>(1));
        return;
    }
    const_istream istm(data, data + size);
    string_ostream sstm;
    uint32_t ttl_mask;
    try {
        ttl_mask = Seq::CmdList::parse(sstm, istm);
    }
    catch (const SyntaxError &err) {
        Log::error("Error parsing startup script: %s\n", err.what());
        auto &emsg = err.msg();
        auto emsgsz = emsg.size();
        auto &line = err.line();
        auto linesz = line.size();
        zmq::message_t zmsg(1 + emsgsz + 1 + linesz + 1 + 4 * 4);
        char *zmsgdata = (char*)zmsg.data();
        *(zmsgdata++) = 1;
        memcpy(zmsgdata, emsg.c_str(), emsgsz + 1);
        zmsgdata += emsgsz + 1;
        memcpy(zmsgdata, line.c_str(), linesz + 1);
        zmsgdata += linesz + 1;
        int lineno = err.lineno();
        memcpy(zmsgdata, &lineno, 4);
        zmsgdata += 4;
        std::array<int,3> cols;
        cols[0] = err.columns(&cols[1], &cols[2]);
        memcpy(zmsgdata, &cols[0], 12);
        send_reply(addr, zmsg);
        return;
    }
    auto ttmpname = m_conf.runtime_dir + "/startup.cmdlist.tmp";
    auto btmpname = m_conf.runtime_dir + "/startup.cmdbin.tmp";
    std::ofstream otstm(ttmpname);
    std::ofstream obstm(btmpname);
    otstm.write(data, size);
    uint32_t ver = 1;
    obstm.write((char*)&ver, 4);
    auto bstr = sstm.get_buf();
    uint64_t len_ns = Seq::CmdList::total_time((uint8_t*)bstr.data(), bstr.size()) * 10;
    obstm.write((char*)&len_ns, 8);
    obstm.write((char*)&ttl_mask, 4);
    obstm.write(bstr.data(), bstr.size());
    if (!obstm.good() || !otstm.good()) {
        Log::error("Cannot save startup file.\n");
        send_reply(addr, ZMQ::bits_msg<uint8_t>(1));
        return;
    }
    otstm.close();
    obstm.close();

    // I'm not really sure what to do if these fails so just ignore error....
    rename(ttmpname.c_str(), (m_conf.runtime_dir + "/startup.cmdlist").c_str());
    rename(btmpname.c_str(), (m_conf.runtime_dir + "/startup.cmdbin").c_str());
    send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
}

void Server::process_zmq()
{
    auto addr = ZMQ::recv_addr(m_zmqsock);

    zmq::message_t msg;
    if (!recv_more(msg))
        goto err;
    if (ZMQ::match(msg, "run_seq")) {
        if (!process_run_seq(addr, false)) {
            goto err;
        }
    }
    else if (ZMQ::match(msg, "run_cmdlist")) {
        if (!process_run_seq(addr, true)) {
            goto err;
        }
    }
    else if (ZMQ::match(msg, "wait_seq")) {
        if (!recv_more(msg) || msg.size() != 17)
            goto err;
        auto id = get_seq_id(msg, 1);
        if (!id)
            goto err;
        uint8_t what = ((uint8_t*)msg.data())[16];
        if (what != 0 && what != 1)
            goto err;
        Log::info("Waiting for sequence %llu\n", (unsigned long long)id);
        if (m_seq_status.empty() || m_seq_status[0].id > id) {
            send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
            goto out;
        }
        for (auto &status: m_seq_status) {
            if (status.id != id)
                continue;
            if (what == 0 && status.flushed) {
                send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
                goto out;
            }
            status.wait.push_back(SeqStatus::Wait{what, std::move(addr)});
            goto out;
        }
        goto err;
    }
    else if (ZMQ::match(msg, "cancel_seq")) {
        bool res;
        if (!recv_more(msg)) {
            Log::info("Canceling all sequences\n");
            res = m_ctrl->cancel_seq(0);
        }
        else if (uint64_t id = get_seq_id(msg)) {
            Log::info("Canceling sequence %llu\n", (unsigned long long)id);
            res = m_ctrl->cancel_seq(id);
        }
        else {
            goto err;
        }
        send_reply(addr, ZMQ::bits_msg(!res));
    }
    else if (ZMQ::match(msg, "state_id")) {
        std::array<uint64_t,2> id{m_ctrl->get_state_id(), m_id};
        nacsDbg("state_id\n");
        send_reply(addr, ZMQ::bits_msg(id));
    }
    else if (ZMQ::match(msg, "override_ttl")) {
        if (!recv_more(msg) || msg.size() != 12)
            goto err;
        uint32_t masks[3];
        memcpy(masks, msg.data(), 12);
        if (masks[0] || masks[1] || masks[2]) {
            Log::info("Override TTLs, [%08x, %08x, %08x]\n",
                      masks[0], masks[1], masks[2]);
        }
        else {
            nacsDbg("get_override_ttl\n");
        }
        for (int i = 0; i < 3; i++)
            m_ctrl->set_ttl_ovr(masks[i], i);
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
        if (!recv_more(msg) || msg.size() != 8)
            goto err;
        uint32_t masks[2];
        memcpy(masks, msg.data(), 8);
        if (masks[0] || masks[1]) {
            Log::info("Set TTLs, [%08x, %08x]\n", masks[0], masks[1]);
        }
        else {
            nacsDbg("get_ttl\n");
        }
        m_ctrl->set_ttl(masks[0], false);
        m_ctrl->set_ttl(masks[1], true);
        m_ctrl->get_ttl([addr{std::move(addr)}, masks, this] (uint32_t v) mutable {
            // The get can arrive faster than the set so manually mask the
            // value to avoid confusion.
            v = (v & ~masks[0]) | masks[1];
            send_reply(addr, ZMQ::bits_msg(v));
        });
    }
    else if (ZMQ::match(msg, "override_dds")) {
        if (!recv_more(msg) || !process_set_dds(msg, true))
            goto err;
        Log::info("Override DDSs\n");
        send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
    }
    else if (ZMQ::match(msg, "get_override_dds")) {
        struct get_override_dds {
            Server *server;
            std::vector<zmq::message_t> addr;
            std::vector<uint8_t> res{};
            ~get_override_dds()
            {
                // This should be called after everyone is done with the callback.
                auto sz = res.size();
                zmq::message_t msg(sz);
                memcpy(msg.data(), &res[0], sz);
                server->send_reply(addr, msg);
            }
        };
        nacsDbg("get_override_dds\n");
        std::shared_ptr<get_override_dds> info(new get_override_dds{this, std::move(addr)});
        for (int i: m_ctrl->get_active_dds()) {
            for (int typ = 0; typ < 3; typ++) {
                m_ctrl->get_dds_ovr(dds_ops[typ], i,
                                    [info, typ, i] (uint32_t v) {
                                        if (v == uint32_t(-1))
                                            return;
                                        push_dds_res(info->res, uint8_t((typ << 6) | i), v);
                                    });
            }
        }
    }
    else if (ZMQ::match(msg, "set_dds")) {
        if (!recv_more(msg) || !process_set_dds(msg, false))
            goto err;
        Log::info("Set DDSs\n");
        send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
    }
    else if (ZMQ::match(msg, "get_dds")) {
        struct get_dds {
            Server *server;
            std::vector<zmq::message_t> addr;
            std::vector<uint8_t> res{};
            ~get_dds()
            {
                // This should be called after everyone is done with the callback.
                auto sz = res.size();
                zmq::message_t msg(sz);
                memcpy(msg.data(), &res[0], sz);
                server->send_reply(addr, msg);
            }
        };
        if (recv_more(msg)) {
            size_t sz = msg.size();
            uint8_t *data = (uint8_t*)msg.data();
            for (size_t i = 0; i < sz; i++) {
                auto chn = data[i];
                if ((chn >> 6) >= 3 || (chn & 0x3f) >= 22) {
                    goto err;
                }
            }
            nacsDbg("get_dds\n");
            std::shared_ptr<get_dds> info(new get_dds{this, std::move(addr)});
            for (size_t i = 0; i < sz; i++) {
                auto chn = data[i];
                m_ctrl->get_dds(dds_ops[chn >> 6], chn & 0x3f,
                                [info, chn] (uint32_t v) {
                                    push_dds_res(info->res, chn, v);
                                });
            }
        }
        else {
            nacsDbg("get_dds\n");
            std::shared_ptr<get_dds> info(new get_dds{this, std::move(addr)});
            for (int i: m_ctrl->get_active_dds()) {
                for (int typ = 0; typ < 3; typ++) {
                    m_ctrl->get_dds(dds_ops[typ], i,
                                    [info, typ, i] (uint32_t v) {
                                        push_dds_res(info->res, uint8_t((typ << 6) | i), v);
                                    });
                }
            }
        }
    }
    else if (ZMQ::match(msg, "reset_dds")) {
        if (!recv_more(msg) || msg.size() != 1)
            goto err;
        int chn = *(char*)msg.data();
        if (chn >= 22)
            goto err;
        Log::info("Reset DDS\n");
        m_ctrl->reset_dds(chn);
        send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
    }
    else if (ZMQ::match(msg, "set_clock")) {
        if (!recv_more(msg) || msg.size() != 1)
            goto err;
        Log::info("Set clock\n");
        uint8_t clock = *(uint8_t*)msg.data();
        m_ctrl->set_clock(clock);
        send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
    }
    else if (ZMQ::match(msg, "get_clock")) {
        m_ctrl->get_clock([addr{std::move(addr)}, this] (uint32_t v) mutable {
            send_reply(addr, ZMQ::bits_msg(uint8_t(v)));
        });
    }
    else if (ZMQ::match(msg, "set_ttl_names")) {
        Log::info("Setting TTL names.\n");
        if (!recv_more(msg) || !process_set_names(msg, m_ttl_names))
            goto err;
        send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
    }
    else if (ZMQ::match(msg, "get_ttl_names")) {
        process_get_names(addr, m_ttl_names);
    }
    else if (ZMQ::match(msg, "set_dds_names")) {
        Log::info("Setting DDS names.\n");
        if (!recv_more(msg) || !process_set_names(msg, m_dds_names))
            goto err;
        send_reply(addr, ZMQ::bits_msg<uint8_t>(0));
    }
    else if (ZMQ::match(msg, "get_dds_names")) {
        process_get_names(addr, m_dds_names);
    }
    else if (ZMQ::match(msg, "get_startup")) {
        std::string str;
        std::ifstream istm(m_conf.runtime_dir + "/startup.cmdlist");
        if (istm.good())
            str.append(std::istreambuf_iterator<char>(istm), {});
        zmq::message_t msg(str.size() + 1);
        memcpy(msg.data(), str.c_str(), str.size() + 1);
        send_reply(addr, msg);
    }
    else if (ZMQ::match(msg, "set_startup")) {
        if (!recv_more(msg) || msg.size() < 1)
            goto err;
        process_set_startup(addr, msg);
    }
    else {
        goto err;
    }
    goto out;
err:
    Log::warn("Request validation failed.\n");
    send_reply(addr, ZMQ::bits_msg<uint8_t>(1));
out:
    ZMQ::readall(m_zmqsock);
}

}
