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

#include <nacs-utils/number.h>

#include <stdio.h>

int main(int argc, char **argv)
{
    using namespace Molecube;
    if (argc < 3 || argc > 5) {
        printf("Require two to four arguments: test_trigger chn val [trigger_chn [trigger_edge<0:lower, 1:raise>]]\n");
        return 1;
    }
    int fullchn = atoi(argv[1]);
    bool val = atoi(argv[2]) != 0;
    if (fullchn >= 32 * NUM_TTL_BANKS || fullchn < 0) {
        printf("Channel number %d out of range.\n", fullchn);
        return 1;
    }
    auto chn = fullchn % 32;
    auto bank = fullchn / 32;

    auto addr = Pulser::address();
    if (!addr) {
        printf("Cannot find pulser\n");
        return 1;
    }
    int trigger_in = -1;
    bool trigger_raise = 1;
    if (argc >= 4) {
        trigger_in = atoi(argv[3]);
        if (argc == 5)
            trigger_raise = atoi(argv[4]) != 0;
        if (trigger_in >= 256 || trigger_in < 0) {
            printf("Trigger channel number %d out of range.\n", trigger_in);
            return 1;
        }
    }

    Pulser p(addr);
    auto old_ttl = p.cur_ttl(bank);
    uint32_t set_ttl = NaCs::setBit(old_ttl, (uint8_t)chn, val);
    uint32_t set_ttl2 = NaCs::setBit(old_ttl, (uint8_t)chn, !val);
    p.set_hold();
    p.clear_error();
    p.template ttl<false>(set_ttl, 3, bank);
    if (trigger_in >= 0)
        p.template wait_trigger<false>((uint8_t)trigger_in, trigger_raise, 10000);
    p.template ttl<false>(set_ttl2, 3, bank);
    p.release_hold();
    while (!p.is_finished()) {
    }
    auto new_ttl = p.cur_ttl(bank);
    printf("New TTL value: %08x\n", new_ttl);
    assert(p.cur_ttl(bank) == new_ttl);
    printf("Status: %08x\n", p.timing_status());
    return 0;
}
