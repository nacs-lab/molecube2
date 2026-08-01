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
#include <vector>

#include <nacs-utils/log.h>
#include <nacs-seq/zynq/pulse_time.h>

template<typename P>
void test_pulser(P &p)
{
    using namespace std::literals;
    using namespace Molecube;

    // Test TTL masks
    printf("  Testing TTL masks and loopback register\n");
    for (int i = 0; i < 32; i++) {
        uint32_t v = 1u << i;
        p.set_loopback_reg(v);
        assert(p.loopback_reg() == v);
        for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
            p.set_ttl_himask(v, bank);
            assert(p.ttl_himask(bank) == v);
            p.set_ttl_lomask(v, bank);
            assert(p.ttl_lomask(bank) == v);
        }
    }
    std::this_thread::sleep_for(1ms);
    p.set_loopback_reg(0xffffffff);
    assert(p.loopback_reg() == 0xffffffff);
    p.set_loopback_reg(0);
    assert(p.loopback_reg() == 0);
    for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
        p.set_ttl_himask(0xffffffff, bank);
        assert(p.ttl_himask(bank) == 0xffffffff);
        p.set_ttl_himask(0, bank);
        assert(p.ttl_himask(bank) == 0);
        p.set_ttl_lomask(0xffffffff, bank);
        assert(p.ttl_lomask(bank) == 0xffffffff);
        p.set_ttl_lomask(0, bank);
        assert(p.ttl_lomask(bank) == 0);
    }

    p.toggle_init();

    // Test TTL and loopback pulse
    printf("  Testing TTL and loopback pulse\n");
    p.release_hold();
    for (int i = 0; i < 32; i++) {
        uint32_t v = 1u << i;
        uint32_t vl = v * 15 + 0x12345678;
        for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
            p.template ttl<false>(v, 10, bank);
        }
        p.template loopback<false>(vl);
        assert(p.get_result() == vl);
        for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
            assert(p.cur_ttl(bank) == v);
        }
    }
    for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
        p.template ttl<false>(0xffffffff, 10, bank);
    }
    p.template loopback<false>(0);
    assert(p.get_result() == 0);
    for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
        assert(p.cur_ttl(bank) == 0xffffffff);
    }
    for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
        p.template ttl<false>(0, 10, bank);
    }
    p.template loopback<false>(0xffffffff);
    assert(p.get_result() == 0xffffffff);
    for (int bank = 0; bank < NUM_TTL_BANKS; bank++) {
        assert(p.cur_ttl(bank) == 0);
    }

    // Test hold and release
    printf("  Testing hold and release\n");
    p.set_hold();
    p.template ttl<false>(345, 10, 0);
    p.template loopback<false>(888);
    std::this_thread::sleep_for(10ms);
    uint32_t res0;
    assert(!p.try_get_result(res0));
    assert(p.cur_ttl(0) == 0);
    p.release_hold();
    assert(p.get_result() == 888);
    assert(p.cur_ttl(0) == 345);

    assert(p.is_finished());

    p.template ttl<false>(0, 10, 0);

    while (!p.is_finished()) {
    }

    // Test loopback and clock
    printf("  Testing loopback and clock\n");
    p.toggle_init();
    p.release_hold();
    assert(p.cur_clock() == 255);
    for (int i = 0; i < 256; i++) {
        uint8_t vc = uint8_t(i);
        uint32_t vl = i * 15 + 12389 + i / 2 + (i << 20);
        p.template clock<false>(vc);
        p.template loopback<false>(vl);
        assert(p.get_result() == vl);
        assert(p.cur_clock() == vc);
    }

    // Timing error
    printf("  Testing timing error\n");
    assert(p.timing_ok());
    p.template wait<true>(3);
    std::this_thread::sleep_for(10ms);
    p.template wait<true>(3);
    std::this_thread::sleep_for(10ms);
    assert(!p.timing_ok());
    p.clear_error();
    p.template loopback<false>(1);
    assert(p.get_result() == 1);
    assert(p.timing_ok());

    // Test auto release
    printf("  Testing auto release\n");
    p.toggle_init();
    p.set_hold();
    for (int i = 0; i < 4095; i++) {
        p.template wait<true>(5);
    }
    for (int i = 0; i < 8; i++) {
        p.template wait<true>(1000);
    }
    p.template wait<false>(3);
    assert(p.timing_ok());
    while (!p.is_finished()) {
    }
    assert(p.timing_ok());

    // Test DDS controller read/write/timing (does not require a working DDS)
    printf("  Testing DDS controller read/write/timing\n");
    p.template dds_set_freq<false>(0, 0);
    p.template dds_set_amp<false>(0, 0);
    while (!p.is_finished()) {
    }
    for (int i = 0; i < 256; i++) {
        uint32_t vl = (i << 20) | i;
        p.template dds_get_freq<false>(0);
        p.template loopback<false>(vl);
        p.template dds_get_amp<false>(0);
        p.template loopback<false>(vl);
        assert(p.get_result() == 0);
        assert(p.get_result() == vl);
        assert(p.get_result() == 0);
        assert(p.get_result() == vl);
    }
}

int main()
{
    if (auto addr = Molecube::Pulser::address()) {
        printf("Real pulser:\n");
        Molecube::Pulser p(addr);
        test_pulser(p);
    }
    else {
        NaCs::Log::warn("Pulse not enabled!\n");
    }

    printf("Dummy pulser:\n");
    Molecube::DummyPulser dp;
    test_pulser(dp);

    return 0;
}
