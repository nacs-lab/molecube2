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
#include <stdlib.h>

#include <iostream>
#include <vector>

int main(int argc, char **argv)
{
    bool force = false;
    if (argc == 2)
        force = atoi(argv[1]) != 0;

    auto addr = Molecube::Pulser::address();
    if (!addr) {
        printf("Cannot find pulser\n");
        return 1;
    }
    Molecube::Pulser p(addr);

    std::vector<int> exist;
    std::vector<int> not_exist;

    for (int i = 0; i < 22; i++) {
        if (!p.dds_exists(i)) {
            not_exist.push_back(i);
            continue;
        }
        exist.push_back(i);
        if (p.check_dds(i, force)) {
            printf("Channel %d: (re)initialized.\n", i);
        }
        else {
            printf("Channel %d: already initialized.\n", i);
        }
        p.dump_dds(std::cout, i);
    }

    if (!exist.empty()) {
        printf("Existing DDS [%zd]: %d", exist.size(), exist[0]);
        for (size_t i = 1; i < exist.size(); i++)
            printf(", %d", exist[i]);
        printf("\n");
    }
    if (!not_exist.empty()) {
        printf("Non-existing DDS [%zd]: %d", not_exist.size(), not_exist[0]);
        for (size_t i = 1; i < not_exist.size(); i++)
            printf(", %d", not_exist[i]);
        printf("\n");
    }

    return 0;
}
