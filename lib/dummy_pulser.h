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

#ifndef LIBMOLECUBE_DUMMY_PULSER_H
#define LIBMOLECUBE_DUMMY_PULSER_H

#include <nacs-utils/utils.h>

#include <atomic>

namespace Molecube {

using namespace NaCs;

/**
 * This is a dummy implementation of `Pulser`
 * that provide the same API and can be used for testing.
 *
 * To simplify the implementation, functions that require access to the command or result
 * fifo's are assumed to be called only from a single thread.
 * Hold/release/init/timing functions should also only be called from this thread.
 * Other functions (current ttl, clock) can be called from any threads.
 */
class DummyPulser {
    DummyPulser(const DummyPulser&) = delete;
    void operator=(const DummyPulser&) = delete;

public:
    // Read
    inline uint32_t ttl_himask() const
    {
        return m_ttl_hi.load();
    }
    inline uint32_t ttl_lomask() const
    {
        return m_ttl_lo.load();
    }

    // Write
    inline void set_ttl_himask(uint32_t high_mask)
    {
        m_ttl_hi.store(high_mask);
    }
    inline void set_ttl_lomask(uint32_t low_mask)
    {
        m_ttl_lo.store(low_mask);
    }
    DummyPulser();

private:
    std::atomic<uint32_t> m_ttl_hi{0};
    std::atomic<uint32_t> m_ttl_lo{0};
};

}

#endif
