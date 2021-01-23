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
    printf("TTL masks and loopback register\n");
    for (int i = 0; i < 32; i++) {
        uint32_t v = 1u << i;
        p.set_loopback_reg(v);
        assert(p.loopback_reg() == v);
        p.set_ttl_himask(v);
        assert(p.ttl_himask() == v);
        p.set_ttl_lomask(v);
        assert(p.ttl_lomask() == v);
    }
    std::this_thread::sleep_for(1ms);
    p.set_loopback_reg(0xffffffff);
    assert(p.loopback_reg() == 0xffffffff);
    p.set_loopback_reg(0);
    assert(p.loopback_reg() == 0);
    p.set_ttl_himask(0xffffffff);
    assert(p.ttl_himask() == 0xffffffff);
    p.set_ttl_himask(0);
    assert(p.ttl_himask() == 0);
    p.set_ttl_lomask(0xffffffff);
    assert(p.ttl_lomask() == 0xffffffff);
    p.set_ttl_lomask(0);
    assert(p.ttl_lomask() == 0);

    p.toggle_init();

    uint32_t inst_word_count = 0;
    uint32_t inst_count = 0;
    uint32_t ttl_count = 0;
    uint32_t loopback_count = 0;
    uint32_t wait_count = 0;
    uint32_t clock_count = 0;
    auto inst_queued = [&] (uint32_t n=1) {
        inst_word_count += n * 2;
        assert(p.inst_word_count() == inst_word_count);
    };
    auto inst_finished = [&] (uint32_t n) {
        inst_count += n;
    };
    auto check_inst = [&] {
        assert(p.inst_count() == inst_count);
        assert(p.ttl_count() == ttl_count);
        assert(p.loopback_count() == loopback_count);
        assert(p.wait_count() == wait_count);
        assert(p.clock_count() == clock_count);
    };
    auto ttl_finished = [&] (uint32_t n=1) {
        ttl_count += n;
        inst_finished(n);
    };
    auto loopback_finished = [&] (uint32_t n=1) {
        loopback_count += n;
        inst_finished(n);
    };
    auto wait_finished = [&] (uint32_t n=1) {
        wait_count += n;
        inst_finished(n);
    };
    auto clock_finished = [&] (uint32_t n=1) {
        clock_count += n;
        inst_finished(n);
    };
    auto reset_count = [&] {
        inst_word_count = 0;
        inst_count = 0;
        ttl_count = 0;
        loopback_count = 0;
        wait_count = 0;
        clock_count = 0;
        assert(p.inst_word_count() == 0);
        check_inst();
    };
    reset_count();

    // Test TTL and loopback pulse
    printf("Testing TTL and loopback pulse\n");
    p.release_hold();
    for (int i = 0; i < 32; i++) {
        uint32_t v = 1u << i;
        uint32_t vl = v * 15 + 0x12345678;
        p.template ttl<false>(v, 10);
        inst_queued();
        p.template loopback<false>(vl);
        inst_queued();
        assert(p.get_result() == vl);
        assert(p.cur_ttl() == v);
        ttl_finished();
        loopback_finished();
        check_inst();
    }
    p.template ttl<false>(0xffffffff, 10);
    inst_queued();
    p.template loopback<false>(0);
    inst_queued();
    assert(p.get_result() == 0);
    assert(p.cur_ttl() == 0xffffffff);
    ttl_finished();
    loopback_finished();
    check_inst();
    p.template ttl<false>(0, 10);
    inst_queued();
    p.template loopback<false>(0xffffffff);
    inst_queued();
    assert(p.get_result() == 0xffffffff);
    assert(p.cur_ttl() == 0);
    ttl_finished();
    loopback_finished();
    check_inst();

    // Test hold and release
    printf("Testing hold and release\n");
    p.set_hold();
    p.template ttl<false>(345, 10);
    inst_queued();
    p.template loopback<false>(888);
    inst_queued();
    check_inst();
    std::this_thread::sleep_for(10ms);
    check_inst();
    uint32_t res0;
    assert(!p.try_get_result(res0));
    assert(p.cur_ttl() == 0);
    p.release_hold();
    assert(p.get_result() == 888);
    assert(p.cur_ttl() == 345);
    ttl_finished();
    loopback_finished();
    check_inst();

    assert(p.is_finished());

    p.template ttl<false>(0, 10);
    inst_queued();

    while (!p.is_finished()) {
    }
    ttl_finished();
    check_inst();

    // Test loopback and clock
    printf("Testing loopback and clock\n");
    p.toggle_init();
    reset_count();
    p.release_hold();
    assert(p.cur_clock() == 255);
    for (int i = 0; i < 256; i++) {
        uint8_t vc = uint8_t(i);
        uint32_t vl = i * 15 + 12389 + i / 2 + (i << 20);
        p.template clock<false>(vc);
        inst_queued();
        p.template loopback<false>(vl);
        inst_queued();
        assert(p.get_result() == vl);
        clock_finished();
        loopback_finished();
        check_inst();
        assert(p.cur_clock() == vc);
    }

    // Timing error
    printf("Testing timing error\n");
    assert(p.timing_ok());
    assert(p.underflow_cycle() == 0);
    p.template wait<true>(1);
    inst_queued();
    std::this_thread::sleep_for(10ms);
    wait_finished();
    check_inst();
    p.template wait<true>(1);
    inst_queued();
    std::this_thread::sleep_for(10ms);
    wait_finished();
    check_inst();
    assert(!p.timing_ok());
    assert(p.underflow_cycle() > 1000000);
    p.clear_error();
    inst_queued();
    p.template loopback<false>(1);
    inst_queued();
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
