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

struct DMABuff {
    void *virt_addr;
    void *phy_addr;
    size_t size;
    DMABuff(void *virt_addr, size_t size)
        : virt_addr(virt_addr),
          phy_addr(Kernel::bufferPhyAddr(virt_addr)),
          size(size)
    {
    }

    static DMABuff alloc_dma(size_t size)
    {
        return DMABuff{Kernel::allocDMABuffer(size), size};
    }
    static DMABuff alloc_ocm(size_t size)
    {
        return DMABuff{Kernel::allocOCMBuffer(size), size};
    }

    void run_dma(Molecube::Pulser &p, int idx, size_t size=-1) const
    {
        if (size == (size_t)-1)
            size = this->size;
        p.write(idx, (unsigned)phy_addr);
        p.write(idx, size / (16 * 8) - 1);
    }

    void clear_cache()
    {
        __builtin___clear_cache(virt_addr, (char*)virt_addr + size);
    }
};

static void dma_dbg_print(Molecube::Pulser &p, const char *name=nullptr)
{
    if (!name)
        name = "dma status";
    printf("%s: %d, %d, %d, %08x, %08x, %08x\n", name,
           p.read(0x31), p.read(0x32), p.read(0x33),
           p.read(0x34), p.read(0x35), p.read(0x36));
}

static void do_rep_dma(Molecube::Pulser &p, const std::vector<DMABuff> &buffs,
                       int idx, int rep, size_t size = -1)
{
    for (int i = 0; i < rep; i++) {
        for (auto &buff: buffs) {
            buff.run_dma(p, idx, size);
        }
    }
}

static void bench_dma(Molecube::Pulser &p, const std::vector<DMABuff> &buffs,
                       int idx, int rep, size_t size = -1)
{
    dma_dbg_print(p, "pre-transfer");
    auto start_count = p.read(0x31);
    std::this_thread::sleep_for(10ms);
    NaCs::Timer timer;
    do_rep_dma(p, buffs, idx, rep, size);
    dma_dbg_print(p, "post-transfer");
    while (p.read(0x31) != start_count + rep * 2)
        std::this_thread::yield();
    timer.print();
    dma_dbg_print(p, "post-wait");
}

int main()
{
    auto addr = Molecube::Pulser::address();
    printf("%p\n", addr);
    Molecube::Pulser p(addr);
    auto buff1 = DMABuff::alloc_dma(16 * 4096);
    auto buff2 = DMABuff::alloc_dma(16 * 4096);
    auto buff3 = DMABuff::alloc_ocm(16 * 4096);
    auto buff4 = DMABuff::alloc_ocm(16 * 4096);
    printf("%p, %p, %p, %p\n", buff1.virt_addr, buff2.virt_addr,
           buff3.virt_addr, buff4.virt_addr);
    buff1.clear_cache();
    buff2.clear_cache();
    buff3.clear_cache();
    buff4.clear_cache();
    printf("%p, %p, %p, %p\n", buff1.phy_addr, buff2.phy_addr,
           buff3.phy_addr, buff4.phy_addr);

    printf("Main Memory HP\n");
    bench_dma(p, {buff1, buff2}, 0x20, 10);
    printf("OCM HP\n");
    bench_dma(p, {buff3, buff4}, 0x20, 10);

    printf("Main Memory HP\n");
    bench_dma(p, {buff1, buff2}, 0x20, 10);
    printf("OCM HP\n");
    bench_dma(p, {buff3, buff4}, 0x20, 10);

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
    // bench_dma(p, {buff1, buff2}, 0x21, 10);
    // printf("OCM ACP\n");
    // bench_dma(p, {buff3, buff4}, 0x21, 10);
    if (buff1.virt_addr)
        Kernel::freeDMABuffer(buff1.virt_addr, 16 * 4096);
    if (buff2.virt_addr)
        Kernel::freeDMABuffer(buff2.virt_addr, 16 * 4096);
    if (buff3.virt_addr)
        Kernel::freeOCMBuffer(buff3.virt_addr, 16 * 4096);
    if (buff4.virt_addr)
        Kernel::freeOCMBuffer(buff4.virt_addr, 16 * 4096);
    return 0;
}
