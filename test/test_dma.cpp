/*************************************************************************
 *   Copyright (c) 2021 - 2021 Yichao Yu <yyc1992@gmail.com>             *
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

#include <nacs-utils/mem.h>
#include <nacs-utils/timer.h>
#include <nacs-kernel/devctl.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include <sys/syscall.h>

#include <inttypes.h>
#include <unistd.h>

using namespace NaCs;
using namespace Molecube;
using namespace std::literals;

static void do_dma(Molecube::Pulser &p, void *buff1, void *buff2, int idx, int rep)
{
    printf("pre-transfer: %d, %d, %d, %08x, %08x, %08x\n", p.read(0x31), p.read(0x32), p.read(0x33), p.read(0x34), p.read(0x35), p.read(0x36));
    auto start_count = p.read(0x31);
    std::this_thread::sleep_for(10ms);
    NaCs::Timer timer;
    for (int i = 0; i < rep; i++) {
        p.write(idx, (unsigned)buff1);
        p.write(idx, 512 - 1);
        p.write(idx, (unsigned)buff2);
        p.write(idx, 512 - 1);
    }
    printf("post-transfer: %d, %d, %d, %08x, %08x, %08x\n", p.read(0x31), p.read(0x32), p.read(0x33), p.read(0x34), p.read(0x35), p.read(0x36));
    // for (int i = 0; i < 5; i++) {
    //     printf("post-transfer: %d, %d, %d, %08x, %08x, %08x\n", p.read(0x31), p.read(0x32), p.read(0x33), p.read(0x34), p.read(0x35), p.read(0x36));
    // }
    while (p.read(0x31) != start_count + rep * 2)
        std::this_thread::yield();
    timer.print();
    printf("post-wait: %d, %d, %d, %08x, %08x, %08x\n", p.read(0x31), p.read(0x32), p.read(0x33), p.read(0x34), p.read(0x35), p.read(0x36));
}

int main()
{
    auto addr = Molecube::Pulser::address();
    printf("%p\n", addr);
    Molecube::Pulser p(addr);
    auto buff1 = Kernel::allocDMABuffer(16 * 4096);
    auto buff2 = Kernel::allocDMABuffer(16 * 4096);
    auto buff3 = Kernel::allocOCMBuffer(16 * 4096);
    auto buff4 = Kernel::allocOCMBuffer(16 * 4096);
    printf("%p, %p, %p, %p\n", buff1, buff2, buff3, buff4);
    __builtin___clear_cache(buff1, (char*)buff1 + 4096 * 16);
    __builtin___clear_cache(buff2, (char*)buff2 + 4096 * 16);
    __builtin___clear_cache(buff3, (char*)buff3 + 4096 * 16);
    __builtin___clear_cache(buff4, (char*)buff4 + 4096 * 16);
    auto addr1 = Kernel::bufferPhyAddr(buff1);
    auto addr2 = Kernel::bufferPhyAddr(buff2);
    auto addr3 = Kernel::bufferPhyAddr(buff3);
    auto addr4 = Kernel::bufferPhyAddr(buff4);
    printf("%p, %p, %p, %p\n", addr1, addr2, addr3, addr4);

    printf("Main Memory HP\n");
    do_dma(p, addr1, addr2, 0x20, 10);
    printf("OCM HP\n");
    do_dma(p, addr3, addr4, 0x20, 10);

    printf("Main Memory HP\n");
    do_dma(p, addr1, addr2, 0x20, 10);
    printf("OCM HP\n");
    do_dma(p, addr3, addr4, 0x20, 10);

    // {
    //     printf("Get Version\n");
    //     Timer timer;
    //     for (int i = 0; i < 1000000; i++) {
    //         Kernel::getDriverVersion();
    //     }
    //     timer.print();
    // }

    // {
    //     printf("Flush main memory\n");
    //     Timer timer;
    //     for (int i = 0; i < 1000000; i++) {
    //         __builtin___clear_cache(buff1, (char*)buff1 + 4096 * 16);
    //     }
    //     timer.print();
    // }

    // {
    //     printf("Flush OCM\n");
    //     Timer timer;
    //     for (int i = 0; i < 1000000; i++) {
    //         __builtin___clear_cache(buff3, (char*)buff3 + 4096 * 16);
    //     }
    //     timer.print();
    // }

    // printf("Main Memory ACP\n");
    // do_dma(p, addr1, addr2, 0x21, 1);
    // printf("OCM ACP\n");
    // do_dma(p, addr3, addr4, 0x21, 1);
    if (buff1)
        Kernel::freeDMABuffer(buff1, 16 * 4096);
    if (buff2)
        Kernel::freeDMABuffer(buff2, 16 * 4096);
    if (buff3)
        Kernel::freeOCMBuffer(buff3, 16 * 4096);
    if (buff4)
        Kernel::freeOCMBuffer(buff4, 16 * 4096);
    return 0;
}
