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

#include <nacs-utils/errors.h>
#include <nacs-utils/mem.h>
#include <nacs-utils/timer.h>
#include <nacs-kernel/device.h>
#include <nacs-kernel/devctl.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

#include <sys/syscall.h>

#include <fcntl.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace NaCs;
using namespace Molecube;
using namespace std::literals;

typedef struct {
    unsigned long addr;
    unsigned long size;
    int l1_only: 1;
} _knacs_dma_buff_t;

static std::random_device rd;

static int knacs_fd = open("/dev/knacs", O_RDWR | O_SYNC);

static void flush_dma_buff(void *addr, size_t size, bool l1_only=false)
{
    _knacs_dma_buff_t buff{(unsigned long)addr, (unsigned long)size, l1_only};
    checkErrno(ioctl(knacs_fd, _IOR('y', 2, _knacs_dma_buff_t), &buff));
}

#include "crc32c_table.h"

static uint32_t crc32c(uint32_t crci, const char *buf, size_t len)
{
    uintptr_t crc = crci ^ 0xffffffff;
    while (len && ((uintptr_t)buf & 7) != 0) {
        crc = crc32c_table[0][(crc ^ *buf++) & 0xff] ^ (crc >> 8);
        len--;
    }
    while (len >= 8) {
        uint32_t *p = (uint32_t*)buf;
        crc ^= p[0];
        uint32_t hi = p[1];
        crc = crc32c_table[7][crc & 0xff] ^
            crc32c_table[6][(crc >> 8) & 0xff] ^
            crc32c_table[5][(crc >> 16) & 0xff] ^
            crc32c_table[4][(crc >> 24) & 0xff] ^
            crc32c_table[3][hi & 0xff] ^
            crc32c_table[2][(hi >> 8) & 0xff] ^
            crc32c_table[1][(hi >> 16) & 0xff] ^
            crc32c_table[0][hi >> 24];
        buf += 8;
        len -= 8;
    }
    while (len) {
        crc = crc32c_table[0][(crc ^ *buf++) & 0xff] ^ (crc >> 8);
        len--;
    }
    return (uint32_t)crc ^ 0xffffffff;
}

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

    void rand_fill() const
    {
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<unsigned> distrib;

        auto ptr = (unsigned*)virt_addr;
        for (unsigned i = 0; i < size / sizeof(unsigned); i++) {
            ptr[i] = distrib(gen);
        }
    }

    uint32_t crc32c_sw(size_t size=-1) const
    {
        if (size == (size_t)-1)
            size = this->size;
        return crc32c(0, (const char*)virt_addr, size);
    }

    uint32_t crc32c_dma(Molecube::Pulser &p, int idx, size_t size=-1) const
    {
        auto start_count = p.read(0x31);
        run_dma(p, idx, size);
        while (p.read(0x31) != start_count + 1)
            std::this_thread::yield();
        return p.read(idx + 0x14);
    }

    void flush_dma(bool l1_only=false)
    {
        flush_dma_buff(virt_addr, size, l1_only);
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

__attribute__((unused))
static void bench_dma(Molecube::Pulser &p, const std::vector<DMABuff> &buffs,
                       int idx, int rep, size_t size = -1)
{
    dma_dbg_print(p, "pre-transfer");
    auto start_count = p.read(0x31);
    std::this_thread::sleep_for(10ms);
    NaCs::Timer timer;
    int j = 0;
    auto last_count = start_count;
    for (int i = 0; i < rep; i++) {
        for (auto &buff: buffs) {
            if (last_count + 8 < start_count + j) {
                while ((last_count = p.read(0x31)) + 8 < start_count + j) {
                    std::this_thread::yield();
                }
            }
            j++;
            buff.run_dma(p, idx, size);
        }
    }
    dma_dbg_print(p, "post-transfer");
    while (p.read(0x31) != start_count + rep * buffs.size())
        std::this_thread::yield();
    timer.print();
    dma_dbg_print(p, "post-wait");
}

int main()
{
    Kernel::init(knacs_fd);
    auto addr = Molecube::Pulser::address();
    printf("%p\n", addr);
    Molecube::Pulser p(addr);
    auto buff1 = DMABuff::alloc_dma(16 * 4096);
    auto buff2 = DMABuff::alloc_dma(16 * 4096);
    auto buff3 = DMABuff::alloc_ocm(16 * 4096);
    auto buff4 = DMABuff::alloc_ocm(16 * 4096);
    printf("%p, %p, %p, %p\n", buff1.virt_addr, buff2.virt_addr,
           buff3.virt_addr, buff4.virt_addr);
    buff1.flush_dma();
    buff2.flush_dma();
    buff3.flush_dma();
    buff4.flush_dma();
    printf("%p, %p, %p, %p\n", buff1.phy_addr, buff2.phy_addr,
           buff3.phy_addr, buff4.phy_addr);

    printf("Main Memory HP\n");
    bench_dma(p, {buff1}, 0x20, 1000);
    printf("OCM HP\n");
    bench_dma(p, {buff3}, 0x20, 1000);

    printf("Main Memory ACP\n");
    bench_dma(p, {buff1}, 0x21, 1000);
    printf("OCM ACP\n");
    bench_dma(p, {buff3}, 0x21, 1000);

    // {
    //     printf("Yield\n");
    //     Timer timer;
    //     for (int i = 0; i < 1000; i++) {
    //         std::this_thread::yield();
    //     }
    //     timer.print();
    // }

    // {
    //     printf("Sleep 1us\n");
    //     Timer timer;
    //     for (int i = 0; i < 1000; i++) {
    //         std::this_thread::sleep_for(1us);
    //     }
    //     timer.print();
    // }

    // {
    //     printf("Sleep 1ns\n");
    //     Timer timer;
    //     for (int i = 0; i < 1000; i++) {
    //         std::this_thread::sleep_for(1ns);
    //     }
    //     timer.print();
    // }

    // {
    //     printf("Yield\n");
    //     Timer timer;
    //     for (int i = 0; i < 1000; i++) {
    //         __asm__ volatile ("yield" ::: "memory");
    //     }
    //     timer.print();
    // }

    // {
    //     printf("Noop\n");
    //     Timer timer;
    //     for (int i = 0; i < 1000; i++) {
    //         __asm__ volatile ("nop" ::: "memory");
    //     }
    //     timer.print();
    // }

    {
        printf("Get Version\n");
        Timer timer;
        for (int i = 0; i < 1000; i++) {
            Kernel::getDriverVersion();
        }
        timer.print();
    }

    {
        printf("Flush main memory\n");
        Timer timer;
        for (int i = 0; i < 1000; i++) {
            buff1.flush_dma();
        }
        timer.print();
    }

    {
        printf("Flush OCM\n");
        Timer timer;
        for (int i = 0; i < 1000; i++) {
            buff3.flush_dma();
        }
        timer.print();
    }

    {
        printf("Flush main memory l1\n");
        Timer timer;
        for (int i = 0; i < 1000; i++) {
            buff1.flush_dma(true);
        }
        timer.print();
    }

    {
        printf("Flush OCM l1\n");
        Timer timer;
        for (int i = 0; i < 1000; i++) {
            buff3.flush_dma(true);
        }
        timer.print();
    }

    {
        printf("Main Memory CRC32C\n");
        buff1.rand_fill();
        auto crc = buff1.crc32c_sw();
        auto crc1 = buff1.crc32c_dma(p, 0x20);
        buff1.flush_dma();
        auto crc2 = buff1.crc32c_dma(p, 0x20);
        printf("  %08x, %08x, %08x\n", crc, crc1, crc2);
    }

    {
        printf("OCM CRC32C\n");
        buff3.rand_fill();
        auto crc = buff3.crc32c_sw();
        auto crc1 = buff3.crc32c_dma(p, 0x20);
        buff3.flush_dma();
        auto crc2 = buff3.crc32c_dma(p, 0x20);
        printf("  %08x, %08x, %08x\n", crc, crc1, crc2);
    }

    {
        printf("Main Memory ACP CRC32C\n");
        buff1.rand_fill();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto crc = buff1.crc32c_sw();
        auto crc1 = buff1.crc32c_dma(p, 0x21);
        buff1.flush_dma(true);
        auto crc2 = buff1.crc32c_dma(p, 0x21);
        printf("  %08x, %08x, %08x\n", crc, crc1, crc2);
    }

    {
        printf("OCM ACP CRC32C\n");
        buff3.rand_fill();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto crc = buff3.crc32c_sw();
        auto crc1 = buff3.crc32c_dma(p, 0x21);
        buff3.flush_dma(true);
        auto crc2 = buff3.crc32c_dma(p, 0x21);
        printf("  %08x, %08x, %08x\n", crc, crc1, crc2);
    }

    dma_dbg_print(p, "final");

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
