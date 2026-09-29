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

#include "pulser_common.h"
#include "config.h"

#include <nacs-utils/container.h>
#include <nacs-utils/mem.h>
#include <nacs-utils/utils.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

namespace Molecube {

using namespace NaCs;

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
        virtual void set_id(uint64_t)
        {}
        // Right before the sequence starts
        virtual void start(uint64_t)
        {}
        // After all the commands are sent
        virtual void flushed(uint64_t)
        {}
        // After the sequence finished
        virtual void end(uint64_t)
        {}
        // After the sequence is cancelled
        virtual void cancel(uint64_t)
        {}
        virtual ~ReqSeqNotify()
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
    class callback_t {
        template<typename T>
        struct Caller {
            static void call(void *p, uint32_t v)
            {
                (*(T*)p)(v);
            }
        };
    public:
        template<typename T,
                 class=std::enable_if_t<!std::is_same<std::remove_cvref_t<T>,callback_t>::value>>
        callback_t(T &&v)
            : m_ptr(std::forward<T>(v)),
              m_fptr(Caller<std::remove_cvref_t<T>>::call)
        {}
        callback_t(callback_t &&cb)
            : m_ptr(std::move(cb.m_ptr)),
              m_fptr(cb.m_fptr)
        {}
        void operator()(uint32_t v)
        {
            m_fptr(m_ptr.get(), v);
        }
    private:
        AnyPtr m_ptr;
        void (*m_fptr)(void*, uint32_t);
    };
protected:
    /**
     * There are two kinds of requests that can pass through this interface,
     * 1. Stand alone commands.
     *    These are used to change a single state including
     *    setting/getting output values or overrides. These are not timed.
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
        uint32_t operand: 26; // opcode specific encoding (e.g. channel number)
        // DDSFreq/Phase/Amp: operand is channel number
        // TTL (set only): the last two bits are the value to set (0: low, 1: high),
        //   the bits before that specify the bank number and
        //   `val` is the mask of the channels to set.
        uint32_t val; // opcode specific encoding of value.
    };

    static constexpr uint8_t NDDS = 22;

    // Frontend cache and override state of the DDS parameters.
    // Only `overridden` may be read by the worker thread (during a sequence).
    struct DDSParamCache {
        // Whether the parameter is overridden (by the frontend).
        std::atomic<bool> overridden{false};
        // Whether `val` is the current value of the parameter.
        bool cached = false;
        // The current value, which is also the override value if `overridden`.
        uint32_t val = 0;
        // Time when `val` was cached.
        uint64_t t = 0;
        void reset()
        {
            overridden.store(false, std::memory_order_relaxed);
            cached = false;
            val = 0;
            t = 0;
        }
    };
    struct DDSCache {
        // freq, amp, phase
        DDSParamCache params[3];
    };

    enum ReqSeqState {
        SeqCancel = -1,
        SeqInit = 0,
        SeqStart,
        SeqFlushed,
        SeqEnd,
    };
    struct ReqSeq {
        // Sequence ID
        uint64_t id;
        // Sequence length in ns
        uint64_t seq_len_ns;
        // Bytecode or cmdlist
        std::span<const uint8_t> code;
        // TTL's used in the sequence. Only these TTL's will be changed in the sequence.
        std::array<uint32_t,NUM_TTL_BANKS> ttl_mask;
        // version
        uint32_t ver;
        // Whether this is a command list or not. (`false` for bytecode).
        bool is_cmd;
        std::atomic<bool> cancel{false};
        // This is set by the backend to signal change of state.
        // Only `SeqEnd` event is guaranteed to have a accompanied event fd notification.
        std::atomic<ReqSeqState> state{SeqInit};
        ReqSeq(uint64_t id, uint64_t seq_len_ns, std::span<const uint8_t> code,
               const std::array<uint32_t,NUM_TTL_BANKS> &ttl_mask, uint32_t ver, bool is_cmd,
               std::unique_ptr<ReqSeqNotify> _notify, AnyPtr storage)
            : id(id), seq_len_ns(seq_len_ns), code(code),
              ttl_mask(ttl_mask), ver(ver), is_cmd(is_cmd),
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
     * Wait for at most `maxt` nanoseconds. If `maxt < 0`, do not time out.
     *
     * Return false if the backend should exit.
     */
    bool wait(int64_t maxt=-1);

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
     * Clear the backend event notifications and
     * run the callbacks for all sequence events.
     */
    void run_seq_frontend();

    void set_dirty();
    void set_observed();

    void send_cmd(ReqOP op, bool has_res, uint32_t operand, uint32_t val);
    // Pop a finished command from the queue (from the frontend).
    ReqCmd *pop_cmd()
    {
        return m_cmd_queue.pop();
    }
    // Free a command returned by `pop_cmd()`.
    void free_cmd(ReqCmd *cmd)
    {
        m_cmd_alloc.free(cmd);
    }

    // Cache entry of the DDS parameter `op` of channel `chn`.
    DDSParamCache &dds_cache(ReqOP op, int chn)
    {
        assert(op == DDSFreq || op == DDSAmp || op == DDSPhase);
        assert(0 <= chn && chn < NDDS);
        return m_dds_cache[chn].params[op - DDSFreq];
    }
    // Whether the DDS parameter `op` of channel `chn` is overridden.
    // Can be called from the worker thread.
    bool dds_overridden(ReqOP op, int chn)
    {
        return dds_cache(op, chn).overridden.load(std::memory_order_relaxed);
    }

    DDSCache m_dds_cache[NDDS];

    CtrlIFace();
public:
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
    virtual void run_frontend() = 0;

    template<typename... Args>
    uint64_t run_code(bool is_cmd, uint32_t ver, uint64_t seq_len_ns,
                      const std::array<uint32_t,NUM_TTL_BANKS> &ttl_mask,
                      std::span<const uint8_t> code,
                      std::unique_ptr<ReqSeqNotify> notify, Args&&... args)
    {
        return _run_code(is_cmd, ver, seq_len_ns, ttl_mask, code,
                         std::move(notify), AnyPtr(std::forward<Args>(args)...));
    }
    // Cancel the sequence determined by the `id`. `id == 0` means cancel all sequences.
    // Return if any sequences may be cancelled.
    // Note that a cancelled sequence that has been started
    // may not response to the cancellation.
    bool cancel_seq(uint64_t id);

    virtual void set_ttl(int bank, uint32_t mask, bool val) = 0;
    // val = 0 => low
    // val = 1 => high
    // val = 2 => default
    virtual void set_ttl_ovr(int bank, uint32_t mask, int val) = 0;

    virtual uint32_t get_ttl(int bank) = 0;
    // The TTL channels overridden to low and high.
    struct TTLOvr {
        uint32_t lo;
        uint32_t hi;
    };
    virtual TTLOvr get_ttl_ovr(int bank) = 0;

    virtual void set_dds(ReqOP op, int chn, uint32_t val) = 0;
    virtual void set_dds_ovr(ReqOP op, int chn, uint32_t val) = 0;

    virtual void get_dds(ReqOP op, int chn, callback_t cb) = 0;
    virtual void get_dds_ovr(ReqOP op, int chn, callback_t cb) = 0;
    virtual void reset_dds(int chn) = 0;
    virtual void set_dds_timing1(int adsu, int wrlow, int adhd, int fuddl, int fudhd) = 0;
    virtual DDSInstTiming get_dds_inst_timing() const = 0;

    virtual void set_clock(uint8_t val) = 0;
    virtual uint8_t get_clock() = 0;

    virtual bool has_ttl_ovr() = 0;
    bool has_dds_ovr();

    void quit();
    uint64_t get_state_id();

    virtual std::vector<int> get_active_dds() = 0;

    // Return whether there is any sequence or command waiting to be or being processed
    // and whether any of them are finished and ready to be freed/trigger the callbacks.
    std::pair<bool,bool> has_pending();

    // Defined in `controller.cpp`
    static std::unique_ptr<CtrlIFace> create(bool dummy=false,
                                             Config::DMAEnable dma_enable=Config::DMAEnable::Disabled);

private:
    uint64_t _run_code(bool is_cmd, uint32_t ver, uint64_t seq_len_ns,
                       const std::array<uint32_t,NUM_TTL_BANKS> &ttl_mask,
                       std::span<const uint8_t> code,
                       std::unique_ptr<ReqSeqNotify> notify, AnyPtr storage);

    bool m_quit{false};

    // To notify the backend of new requests from the frontend.
    std::mutex m_ftend_lck;
    std::condition_variable m_ftend_evt;

    // Sequence ID counter
    uint64_t m_seq_cnt = 0;
    // State ID counter
    uint64_t m_state_cnt = 0;
    bool m_dirty = false;
    bool m_observed = false;
    bool m_had_seq = false;

    // The queues that send the commands/sequences to the backend (filter) and back.
    FilterQueue<ReqCmd> m_cmd_queue;
    FilterQueue<ReqSeq> m_seq_queue;

    // Cached allocator for efficient allocations
    SmallAllocator<ReqCmd,32> m_cmd_alloc;
    SmallAllocator<ReqSeq,32> m_seq_alloc;

    // Use an event fd for notification from the backend to the frontend
    // since this can be polled in the main loop.
    int m_bkend_evt;
};

}

#endif
