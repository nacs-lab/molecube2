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

#include <nacs-utils/utils.h>

#include <functional>
#include <type_traits>
#include <vector>
#include <mutex>

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
    // Wrapping an arbitrary pointer/object's lifetime
    class Storage {
        // C++20
        template<typename T>
        using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;
        template<typename T>
        struct Destructor {
            static void deleter(void *p)
            {
                delete (T*)p;
            }
        };
        void *ptr = nullptr;
        void (*deleter)(void*) = nullptr;
    public:
        Storage() {}
        Storage(Storage &&other)
        {
            std::swap(ptr, other.ptr);
            std::swap(deleter, other.deleter);
        }
        template<typename T>
        Storage(T *v)
            : ptr((void*)v),
              deleter(Destructor<T>::deleter)
        {
        }
        template<typename T,
                 class = std::enable_if_t<!std::is_lvalue_reference<T>::value &&
                                          !std::is_same<remove_cvref_t<T>,Storage>::value &&
                                          !std::is_pointer<T>::value>>
        Storage(T &&v)
            : Storage(new T(std::move(v)))
        {
        }
        Storage(void *ptr, void (*deleter)(void*))
            : ptr(ptr),
              deleter(deleter)
        {
        }
        Storage(const Storage &other) = delete;
        ~Storage()
        {
            if (deleter && ptr) {
                deleter(ptr);
            }
        }
    };

protected:
    enum ReqOP {
        TTL,
        DDSFreq,
        DDSAmp,
        DDSPhase,
    };
    struct ReqCmd {
        uint8_t opcode: 4;
        uint8_t has_res: 1;
        uint8_t overwrite: 1;
        uint32_t operand: 26;
        uint32_t val;
    };
    struct ReqSeq {
        uint64_t seq_len_ns;
        uint32_t ttl_mask;
        const uint8_t *code;
        size_t code_len;
        bool is_cmd;
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
     *
     * A backend event will be generated if a non `NULL` result is returned.
     */
    ReqCmd *pop_cmd();

    /**
     * Try popping a sequence or command list from the queue.
     */
    ReqSeq *pop_seq();

    /**
     * Finishing a command
     */
    void reply_cmd(ReqCmd *cmd);

    /**
     * Finishing a sequence or command list
     *
     * This will always notify the frontend (by generating a backend event).
     */
    void reply_seq(ReqSeq *seq);

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

    int backend_fd() const
    {
        return m_bkend_evt;
    }
    void clear_backend_event();

private:
    // Sequence push/pop isn't performance critical so just use simple queues
    // and a single lock.
    // The controller thread always operate on the back and the frontend thread
    // always operate on the front.
    std::vector<ReqSeq*> m_seq_reqs{};
    std::vector<ReqSeq*> m_seq_replies{};
    std::mutex m_seq_lock{};

    int m_bkend_evt;
};

}

#endif
