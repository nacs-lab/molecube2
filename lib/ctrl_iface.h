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

#ifndef LIBMOLECUBE_CTRL_IFACE_H
#define LIBMOLECUBE_CTRL_IFACE_H

#include <nacs-utils/container.h>
#include <nacs-utils/mem.h>
#include <nacs-utils/utils.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <type_traits>
#include <vector>

namespace Molecube {

using namespace NaCs;

namespace DDS {

enum Type : uint8_t {
    Freq,
    Amp,
    Phase
};

struct __attribute__((__packed__)) Info {
    uint8_t typ: 2;
    uint8_t id: 6;
    uint32_t val;
};

}

/**
 * This is the class that provides the FIFO for commands and sequences,
 * as well as synchronization between the backend and frontend threads.
 * This also implements coalition (and caching?) of read commands and
 * therefore does not guarantee ordering of read and write commands.
 *
 * An implementation of the controller should inherit this and use the
 * protected functions to recieve requested from the frontend on
 * a controller thread.
 * The public functions are provided for the frontend. The functions are all
 * asynchronous and should be called from a single frontend thread.
 *
 * The controller thread is very latency sensitive so the allocation will mostly
 * be done in the frontend thread.
 */
class CtrlIFace {
protected:
    enum ReqOP {
        TTL,
        DDSFreq,
        DDSAmp,
        DDSPhase,
    };
    enum ReqSeqState {
        SeqInit = 0,
        SeqStart,
        SeqEnd
    };
    struct ReqCmd {
        uint8_t opcode: 4;
        uint8_t has_res: 1;
        uint8_t overwrite: 1;
        uint32_t operand: 26;
        uint32_t val;
    };
    struct ReqSeq {
        uint64_t id;
        uint64_t seq_len_ns;
        const uint8_t *code;
        size_t code_len;
        uint32_t ttl_mask;
        bool is_cmd;
        std::atomic<ReqSeqState> state{SeqInit};
        ReqSeq(uint64_t id, uint64_t seq_len_ns, const uint8_t *code, size_t code_len,
               uint32_t ttl_mask, bool is_cmd, std::function<void()> start_cb,
               std::function<void()> end_cb, AnyPtr storage)
            : id(id), seq_len_ns(seq_len_ns), code(code), code_len(code_len),
              ttl_mask(ttl_mask), is_cmd(is_cmd),
              start_cb(std::move(start_cb)), end_cb(std::move(end_cb)),
              storage(std::move(storage))
        {
        }
    private:
        friend class CtrlIFace;
        ReqSeqState processed_state{SeqInit};
        std::function<void()> start_cb;
        std::function<void()> end_cb;
        AnyPtr storage;
    };
    /**
     * These functions are for the controller implementation to use and
     * is expected to be called from a controller thread.
     */

    /**
     * Wait for a new request when there's no sequence running.
     */
    void wait();

    /**
     * Try popping a command from the queue.
     */
    ReqCmd *get_cmd();

    /**
     * Try popping a sequence or command list from the queue.
     */
    ReqSeq *get_seq();

    /**
     * Finishing a command
     */
    void finish_cmd();

    /**
     * Finishing a sequence or command list
     *
     * This will always notify the frontend (by generating a backend event).
     */
    void finish_seq();

    /**
     * Generate a backend event.
     * This notify the frontend that something it can read have changed.
     * Not all changes will generate this event, only the ones that are not
     * latency critical in the backend will.
     * The frontend is expected to poll the backend periodically
     * if it is waiting for something.
     */
    void backend_event();
public:
    CtrlIFace();
    virtual ~CtrlIFace() {}

    /**
     * Returns the backend event fd that the frontend can poll on.
     */
    int backend_fd() const
    {
        return m_bkend_evt;
    }
    /**
     * Clear the backend event notifications and
     * run the callbacks for all events.
     */
    void run_frontend();

    template<typename... Args>
    uint64_t run_code(bool is_cmd, uint64_t seq_len_ns, uint32_t ttl_mask,
                      const uint8_t *code, size_t code_len,
                      std::function<void()> seq_start,
                      std::function<void()> seq_end, Args&&... args)
    {
        return _run_code(is_cmd, seq_len_ns, ttl_mask, code, code_len,
                         std::move(seq_start), std::move(seq_end),
                         AnyPtr(std::forward<Args>(args)...));
    }

private:
    uint64_t _run_code(bool is_cmd, uint64_t seq_len_ns, uint32_t ttl_mask,
                       const uint8_t *code, size_t code_len,
                       std::function<void()> seq_start,
                       std::function<void()> seq_end, AnyPtr storage);

    uint64_t m_seq_cnt = 0;

    FilterQueue<ReqCmd> m_cmd_queue;
    FilterQueue<ReqSeq> m_seq_queue;

    SmallAllocator<ReqCmd,32> m_cmd_alloc;
    SmallAllocator<ReqSeq,32> m_seq_alloc;

    int m_bkend_evt;
};

}

#endif
