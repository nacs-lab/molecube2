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
    if (argc != 3) {
        printf("Require two arguments: test_set_ttl chn val\n");
        return 1;
    }
    int chn = atoi(argv[1]);
    bool val = atoi(argv[2]) != 0;
    if (chn >= 32 || chn < 0) {
        printf("Channel number %d out of range.\n", chn);
        return 1;
    }

    auto addr = Molecube::Pulser::address();
    if (!addr) {
        printf("Cannot find pulser\n");
        return 1;
    }

    Molecube::Pulser p(addr);
    auto old_ttl = p.cur_ttl();
    uint32_t set_ttl = NaCs::setBit(old_ttl, (uint8_t)chn, val);
    p.template ttl<false>(set_ttl, 3);
    while (!p.is_finished()) {
    }
    auto new_ttl = p.cur_ttl();
    printf("New TTL value: %08x\n" new_ttl);
    assert(p.cur_ttl() == new_ttl);
    return 0;
}
