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
    if (argc != 2) {
        printf("Require two arguments: test_set_clock val\n");
        return 1;
    }
    int val = atoi(argv[1]);
    if (val < 0 || val > 255) {
        printf("Clock value %d out of range.\n", val);
        return 1;
    }

    auto addr = Molecube::Pulser::address();
    if (!addr) {
        printf("Cannot find pulser\n");
        return 1;
    }

    Molecube::Pulser p(addr);
    p.template clock<false>((uint8_t)val);
    while (!p.is_finished()) {
    }
    auto new_clock = p.cur_clock();
    printf("New clock value: %d\n", new_clock);
    assert(new_clock == val);
    return 0;
}
