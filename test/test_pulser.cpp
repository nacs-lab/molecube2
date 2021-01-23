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

#include "../lib/pulser.h"
#include "../lib/dummy_pulser.h"

#include <chrono>
#include <stdio.h>
#include <thread>

#include <nacs-utils/log.h>

template<typename P>
void test_pulser(P &p)
{
    using namespace std::literals;

    // Test TTL masks
    printf("TTL masks\n");
    p.set_ttl_himask(0);
    assert(p.ttl_himask() == 0);
    p.set_ttl_himask(123);
    assert(p.ttl_himask() == 123);
    std::this_thread::sleep_for(1ms);
    p.set_ttl_himask(0);
    assert(p.ttl_himask() == 0);

    p.toggle_init();

    // Test TTL pulse
    printf("Testing TTL pulse\n");
    p.release_hold();
    p.template ttl<false>(0, 10);
    p.template loopback<false>(123);
    assert(p.get_result() == 123);
    assert(p.cur_ttl() == 0);

    // Test hold and release
    printf("Testing hold and release\n");
    p.set_hold();
    p.template ttl<false>(345, 10);
    p.template loopback<false>(888);
    std::this_thread::sleep_for(10ms);
    uint32_t res0;
    assert(!p.try_get_result(res0));
    assert(p.cur_ttl() == 0);
    p.release_hold();
    assert(p.get_result() == 888);
    assert(p.cur_ttl() == 345);

    assert(p.is_finished());

    p.template ttl<false>(0, 10);

    while (!p.is_finished()) {
    }

    // Test loopback and clock
    printf("Testing loopback and clock\n");
    p.toggle_init();
    p.release_hold();
    assert(p.cur_clock() == 255);
    p.template clock<false>(128);
    p.template loopback<false>(222);
    assert(p.get_result() == 222);
    assert(p.cur_clock() == 128);
    p.template clock<false>(255);
    p.template loopback<false>(1);
    assert(p.get_result() == 1);
    assert(p.cur_clock() == 255);

    // Timing error
    printf("Testing timing error\n");
    p.template wait<true>(1);
    std::this_thread::sleep_for(10ms);
    p.template wait<true>(1);
    std::this_thread::sleep_for(10ms);
    assert(!p.timing_ok());
    p.clear_error();
    p.template loopback<false>(1);
    assert(p.get_result() == 1);
    assert(p.timing_ok());
}

int main()
{
    if (auto addr = Molecube::Pulser::address()) {
        Molecube::Pulser p(addr);
        test_pulser(p);
    }
    else {
        NaCs::Log::warn("Pulse not enabled!\n");
    }

    Molecube::DummyPulser dp;
    test_pulser(dp);

    return 0;
}
