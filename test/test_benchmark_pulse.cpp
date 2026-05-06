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
#include <nacs-utils/timer.h>
#include <nacs-seq/zynq/pulse_time.h>

template<typename P>
void test_pulser(P &p)
{
    using namespace std::literals;
    using namespace Molecube;
    NaCs::Timer timer;
    for (int i = 0; i < 10000000; i++) {
        p.set_loopback_reg(i);
    }
    timer.print();

    timer.restart();
    for (int i = 0; i < 10000000; i++) {
        p.loopback_reg();
    }
    timer.print();
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
