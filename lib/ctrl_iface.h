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
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <type_traits>
#include <vector>

namespace Molecube {

using namespace NaCs;

struct DDSState {
    DDSState()
        : freq(-1),
          amp(0),
          amp_enable(0),
          phase_enable(0),
          phase(0)
    {
    }
    uint32_t freq; // 31 bits
    uint16_t amp: 12;
    uint16_t amp_enable: 1;
    uint16_t phase_enable: 1;
    uint16_t phase;
};

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
public:
    class ReqSeqNotify {
        // All the virtual functions are called with the request id.
    public:
        // Right before the sequence starts
        virtual void start(uint64_t)
        {}
        // After all the commands are sent
        virtual void flushed(uint64_t)
        {}
        // After the sequence finished
        virtual void end(uint64_t)
        {}
    };
    // Opcode for stand alone commands.
    enum ReqOP {
        TTL,
        DDSFreq,
        DDSAmp,
        DDSPhase,
        DDSReset,
        Clock
    };
protected:
    /**
     * There are two kinds of requests that can pass through this interface,
     * 1. Stand alone commands.
     *    These are used to change a single state including
     *    setting/getting output values or overwrites. These are not timed.
     *    They can happen concurrently with a currently running sequence.
     * 2. Sequences.
     *    These are timed sequences of operations.
     *    A sequence can either be a "restricted" one that will be used to run
     *    actual experiments (bytecode)
     *    or a more "feature-full" one for testing (cmdlist).
     *    Only one sequence can run at the same time.
     */

    struct ReqCmd {
        uint8_t opcode: 4; // ReqOP
        uint8_t has_res: 1;
        uint8_t is_override: 1; // The value set/get is override
        uint32_t operand: 26; // opcode specific encoding (e.g. channel number)
        uint32_t val; // opcode specific encoding of value.
    };

    struct CmdCache {
        // Update the cache to the new value from the set command
        void set(ReqOP op, uint32_t operand, bool is_override, uint32_t val);
        // Try to get the current cached value. If the cached value is not too old,
        // call the `cb` and return `true`. If not, push the `cb` to the list of cbs.
        // If the list is empty, return `false` signaling that a new query should be sent.
        // If the list is not empty, return `true` since a query
        // should have been queued already.
        bool get(ReqOP op, uint32_t operand, bool is_override,
                 std::function<void(uint32_t)> cb);

    private:
        struct CacheEntry {
            uint64_t t = 0;
            uint32_t val = 0;
            std::vector<std::function<void(uint32_t)>> cbs{};
        };
        static uint32_t cache_key(ReqOP op, uint32_t operand, bool is_override)
        {
            assert(op != Clock || !is_override);
            return uint32_t(op) << 27 | uint32_t(is_override) << 26 | operand;
        }
        std::map<uint32_t,CacheEntry> m_cache;
    };

    enum ReqSeqState {
        SeqInit = 0,
        SeqStart,
        SeqFlushed,
        SeqEnd
    };
    struct ReqSeq {
        // Sequence ID
        uint64_t id;
        // Sequence length in ns
        uint64_t seq_len_ns;
        // Bytecode or cmdlist
        const uint8_t *code;
        // Length of `code`
        size_t code_len;
        // TTL's used in the sequence. Only these TTL's will be changed in the sequence.
        uint32_t ttl_mask;
        // Whether this is a command list or not. (`false` for bytecode).
        bool is_cmd;
        // This is set by the backend to signal change of state.
        // Only `SeqEnd` event is guaranteed to have a accompanied event fd notification.
        std::atomic<ReqSeqState> state{SeqInit};
        ReqSeq(uint64_t id, uint64_t seq_len_ns, const uint8_t *code, size_t code_len,
               uint32_t ttl_mask, bool is_cmd,
               std::unique_ptr<ReqSeqNotify> _notify, AnyPtr storage)
            : id(id), seq_len_ns(seq_len_ns), code(code), code_len(code_len),
              ttl_mask(ttl_mask), is_cmd(is_cmd),
              notify(std::move(_notify)), storage(std::move(storage))
        {
        }
    private:
        friend class CtrlIFace;
        // For keeping track of what callback has been invoked.
        ReqSeqState processed_state{SeqInit};
        std::unique_ptr<ReqSeqNotify> notify;
        // For managing any memory associated with the request from the frontend.
        // Most likely for the `code`.
        AnyPtr storage;
    };
    /**
     * These functions are for the controller implementation to use and
     * is expected to be called from a controller thread.
     */

    /**
     * Wait for a new request when there's no sequence running.
     *
     * Return false if the backend should exit.
     */
    bool wait();

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

    /**
     * Try to concurrently set/get values without sending a command in the queue.
     * The backend should implement this for commands
     * that doesn't need to be synchronized to reduce queue pressure.
     *
     * Both function return true if the set/get is completed.
     */
    virtual bool concurrent_set(ReqOP op, uint32_t operand,
                                bool is_override, uint32_t val)
    {
        (void)op;
        (void)operand;
        (void)is_override;
        (void)val;
        return false;
    }
    virtual bool concurrent_get(ReqOP op, uint32_t operand,
                                bool is_override, uint32_t &val)
    {
        (void)op;
        (void)operand;
        (void)is_override;
        (void)val;
        return false;
    }
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
                      std::unique_ptr<ReqSeqNotify> notify, Args&&... args)
    {
        return _run_code(is_cmd, seq_len_ns, ttl_mask, code, code_len,
                         std::move(notify), AnyPtr(std::forward<Args>(args)...));
    }

    void set_ttl(int chn, bool val);
    void set_ttl_all(uint32_t val);
    void set_ttl_ovrhi(uint32_t val);
    void set_ttl_ovrlo(uint32_t val);

    void get_ttl(std::function<void(uint32_t)> cb);
    void get_ttl_ovrhi(std::function<void(uint32_t)> cb);
    void get_ttl_ovrlo(std::function<void(uint32_t)> cb);

    void set_dds(ReqOP op, int chn, uint32_t val);
    void set_dds_ovr(ReqOP op, int chn, uint32_t val);

    void get_dds(ReqOP op, int chn, std::function<void(uint32_t)> cb);
    void get_dds_ovr(ReqOP op, int chn, std::function<void(uint32_t)> cb);
    void reset_dds(int chn);

    void set_clock(uint32_t val);
    void get_clock(std::function<void(uint32_t)> cb);

    void quit();

    virtual std::vector<int> get_active_dds() = 0;

private:
    uint64_t _run_code(bool is_cmd, uint64_t seq_len_ns, uint32_t ttl_mask,
                       const uint8_t *code, size_t code_len,
                       std::unique_ptr<ReqSeqNotify> notify, AnyPtr storage);

    void send_cmd(const ReqCmd &cmd);
    void send_set_cmd(ReqOP op, uint32_t operand, bool is_override, uint32_t val);
    void send_get_cmd(ReqOP op, uint32_t operand, bool is_override,
                      std::function<void(uint32_t)> cb);

    bool m_quit{false};

    // To notify the backend of new requests from the frontend.
    std::mutex m_ftend_lck;
    std::condition_variable m_ftend_evt;

    // Sequence ID counter
    uint64_t m_seq_cnt = 0;

    // The queues that send the commands/sequences to the backend (filter) and back.
    FilterQueue<ReqCmd> m_cmd_queue;
    FilterQueue<ReqSeq> m_seq_queue;

    // Cached allocator for efficient allocations
    SmallAllocator<ReqCmd,32> m_cmd_alloc;
    SmallAllocator<ReqSeq,32> m_seq_alloc;

    CmdCache m_cmd_cache;

    // Use an event fd for notification from the backend to the frontend
    // since this can be polled in the main loop.
    int m_bkend_evt;
};

}

#endif
