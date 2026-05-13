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
#include <span>
#include <thread>
#include <utility>

#include <inttypes.h>
#include <unistd.h>

using namespace NaCs;
using namespace Molecube;
using namespace std::literals;

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

static std::random_device rd;
static std::mt19937 rand_gen(rd());

template<typename T>
static void rand_fill(std::span<T> buff)
{
    std::uniform_int_distribution<T> distrib;
    for (auto &i: buff) {
        i = distrib(rand_gen);
    }
}

static void dma_dbg_print(Molecube::Pulser &p, const char *name=nullptr)
{
    if (!name)
        name = "dma status";
    printf("%s: %d, %d, %d, %08x, %08x, %08x\n", name,
           p.read(0x31), p.read(0x32), p.read(0x33),
           p.read(0x34), p.read(0x35), p.read(0x36));
}

enum class DMAType : uint8_t {
    HP_DDR,
    HP_DDR_WC,
    HP_OCM,
    HP_OCM_WC,
    ACP_DDR,
    ACP_OCM,
    ACP_OCM_WC,
    ACP_COH_DDR,
    ACP_COH_OCM,
    ACP_COH_OCM_WC,
};

struct DMABuff {
    void *virt_addr;
    void *phy_addr;
    size_t size;
    DMAType type;

    static void *alloc_buff(DMAType type, size_t size)
    {
        switch (type) {
        default:
            assert(false);
        case DMAType::HP_DDR:
        case DMAType::ACP_DDR:
        case DMAType::ACP_COH_DDR:
            return Kernel::allocDMABuffer(size, false);
        case DMAType::HP_DDR_WC:
            return Kernel::allocDMABuffer(size, true);
        case DMAType::HP_OCM:
        case DMAType::ACP_OCM:
        case DMAType::ACP_COH_OCM:
            return Kernel::allocOCMBuffer(size, false);
        case DMAType::HP_OCM_WC:
        case DMAType::ACP_OCM_WC:
        case DMAType::ACP_COH_OCM_WC:
            return Kernel::allocOCMBuffer(size, true);
        }
    }

    friend void swap(DMABuff &b1, DMABuff &b2)
    {
        std::swap(b1.virt_addr, b2.virt_addr);
        std::swap(b1.phy_addr, b2.phy_addr);
        std::swap(b1.size, b2.size);
        std::swap(b1.type, b2.type);
    }

    DMABuff(DMAType type, size_t size)
        : virt_addr(alloc_buff(type, size)),
          phy_addr(Kernel::bufferPhyAddr(virt_addr)),
          size(size),
          type(type)
    {}
    DMABuff()
        : virt_addr(nullptr),
          phy_addr(nullptr),
          size(0),
          type(DMAType::HP_DDR)
    {}
    DMABuff(DMABuff &&other)
        : DMABuff()
    {
        swap(*this, other);
    }
    DMABuff(const DMABuff&) = delete;
    DMABuff &operator=(DMABuff &&other)
    {
        swap(*this, other);
        return *this;
    }
    DMABuff &operator=(const DMABuff &other) = delete;

    ~DMABuff()
    {
        if (!virt_addr)
            return;
        switch (type) {
        default:
            assert(false);
        case DMAType::HP_DDR:
        case DMAType::ACP_DDR:
        case DMAType::ACP_COH_DDR:
        case DMAType::HP_DDR_WC:
            Kernel::freeDMABuffer(virt_addr, size);
            return;
        case DMAType::HP_OCM:
        case DMAType::ACP_OCM:
        case DMAType::ACP_COH_OCM:
        case DMAType::HP_OCM_WC:
        case DMAType::ACP_OCM_WC:
        case DMAType::ACP_COH_OCM_WC:
            Kernel::freeOCMBuffer(virt_addr, size);
            return;
        }
    }
    void _queue(Molecube::Pulser &p, int idx, uint8_t user) const
    {
        p.write(idx, (unsigned)phy_addr);
        p.write(idx, (size / (16 * 8) - 1) | (uint32_t(user) << 24));
    }
    void queue(Molecube::Pulser &p) const
    {
        switch (type) {
        default:
            assert(false);
        case DMAType::HP_DDR:
        case DMAType::HP_DDR_WC:
        case DMAType::HP_OCM:
        case DMAType::HP_OCM_WC:
            return _queue(p, 0x20, 0);
        case DMAType::ACP_DDR:
        case DMAType::ACP_OCM:
        case DMAType::ACP_OCM_WC:
            return _queue(p, 0x21, 0);
        case DMAType::ACP_COH_DDR:
        case DMAType::ACP_COH_OCM:
        case DMAType::ACP_COH_OCM_WC:
            return _queue(p, 0x21, 31);
        }
    }

    void prepare() const
    {
        switch (type) {
        default:
            assert(false);
        case DMAType::HP_DDR:
        case DMAType::HP_OCM:
            return Kernel::cleanCache(virt_addr, size, false);
        case DMAType::ACP_DDR:
        case DMAType::ACP_OCM:
            return Kernel::cleanCache(virt_addr, size, true);
        case DMAType::HP_DDR_WC:
        case DMAType::HP_OCM_WC:
        case DMAType::ACP_OCM_WC:
        case DMAType::ACP_COH_DDR:
        case DMAType::ACP_COH_OCM:
        case DMAType::ACP_COH_OCM_WC:
            asm volatile ("dmb ishst" ::: "memory");
            return;
        }
    }
    void rand_fill() const
    {
        ::rand_fill(std::span((unsigned*)virt_addr, size / sizeof(unsigned)));
    }
    uint32_t crc32c_sw() const
    {
        return crc32c(0, (const char*)virt_addr, size);
    }
    uint32_t crc32c_dma(Molecube::Pulser &p) const
    {
        auto start_count = p.read(0x31);
        queue(p);
        while (p.read(0x31) != start_count + 1)
            std::this_thread::yield();
        switch (type) {
        default:
            assert(false);
        case DMAType::HP_DDR:
        case DMAType::HP_DDR_WC:
        case DMAType::HP_OCM:
        case DMAType::HP_OCM_WC:
            return p.read(0x34);
        case DMAType::ACP_DDR:
        case DMAType::ACP_OCM:
        case DMAType::ACP_OCM_WC:
        case DMAType::ACP_COH_DDR:
        case DMAType::ACP_COH_OCM:
        case DMAType::ACP_COH_OCM_WC:
            return p.read(0x35);
        }
    }
};

static void bench_rep(auto &&cb, int rep, int div, auto &&finish)
{
    Timer timer;
    for (int i = 0; i < rep; i++) {
        cb();
    }
    finish();
    auto dt = double(timer.elapsed()) / rep / div;
    if (dt <= 1300) {
        printf("  %.3f ns\n", dt);
        return;
    }
    dt /= 1000;
    if (dt <= 1300) {
        printf("  %.3f us\n", dt);
        return;
    }
    dt /= 1000;
    if (dt <= 1300) {
        printf("  %.3f ms\n", dt);
        return;
    }
    dt /= 1000;
    printf("  %.3f s\n", dt);
}

template<DMAType type>
__attribute__((unused))
static void bench_dma_only(Molecube::Pulser &p, int nbuffs, size_t size, int rep)
{
    std::vector<DMABuff> buffs(nbuffs);
    for (int i = 0; i < nbuffs; i++) {
        buffs[i] = DMABuff(type, size);
    }
    auto start_count = p.read(0x31);
    int j = 0;
    auto last_count = start_count;
    bench_rep([&] {
        for (auto &buff: buffs) {
            if (last_count + 8 < start_count + j) {
                while ((last_count = p.read(0x31)) + 8 < start_count + j) {
                    std::this_thread::yield();
                }
            }
            j++;
            buff.queue(p);
        }
    }, rep, nbuffs, [&] {
        while (p.read(0x31) != start_count + rep * nbuffs) {
            std::this_thread::yield();
        }
    });
}

template<DMAType type>
__attribute__((unused))
static void bench_pipe(Molecube::Pulser &p, int nbuffs, size_t size, int rep)
{
    std::vector<DMABuff> buffs(nbuffs);
    for (int i = 0; i < nbuffs; i++) {
        buffs[i] = DMABuff(type, size);
    }
    auto max_ahead = nbuffs;
    if (max_ahead > 8)
        max_ahead = 8;
    std::vector<uint32_t> content_buff(size / 4);
    rand_fill(std::span(content_buff));
    auto start_count = p.read(0x31);
    int j = 0;
    auto last_count = start_count;
    bench_rep([&] {
        for (auto &buff: buffs) {
            memcpy(buff.virt_addr, &content_buff[0], size);
            buff.prepare();
            if (last_count + max_ahead < start_count + j) {
                while ((last_count = p.read(0x31)) + max_ahead < start_count + j) {
                    std::this_thread::yield();
                }
            }
            j++;
            buff.queue(p);
        }
    }, rep, nbuffs, [&] {
        while (p.read(0x31) != start_count + rep * nbuffs) {
            std::this_thread::yield();
        }
    });
}

template<DMAType type>
__attribute__((unused))
static void bench_memcpy(int nbuffs, size_t size, int rep)
{
    std::vector<DMABuff> buffs(nbuffs);
    for (int i = 0; i < nbuffs; i++) {
        buffs[i] = DMABuff(type, size);
    }
    std::vector<uint32_t> content_buff(size / 4);
    bench_rep([&] {
        for (auto &buff: buffs) {
            memcpy(buff.virt_addr, &content_buff[0], size);
            asm volatile ("" ::: "memory");
        }
    }, rep, nbuffs, [] {});
}

template<DMAType type>
__attribute__((unused))
static void bench_rand_fill(int nbuffs, size_t size, int rep)
{
    std::vector<DMABuff> buffs(nbuffs);
    for (int i = 0; i < nbuffs; i++) {
        buffs[i] = DMABuff(type, size);
    }
    bench_rep([&] {
        for (auto &buff: buffs) {
            buff.rand_fill();
            asm volatile ("" ::: "memory");
        }
    }, rep, nbuffs, [] {});
}

template<DMAType type>
__attribute__((unused))
static void bench_flush(int nbuffs, size_t size, int rep)
{
    std::vector<DMABuff> buffs(nbuffs);
    for (int i = 0; i < nbuffs; i++) {
        buffs[i] = DMABuff(type, size);
    }
    bench_rep([&] {
        for (auto &buff: buffs) {
            buff.prepare();
        }
    }, rep, nbuffs, [] {});
}

template<DMAType type>
__attribute__((unused))
static void test_crc32c(Molecube::Pulser &p, size_t size, int rep)
{
    DMABuff buff(type, size);
    std::vector<uint32_t> content_buff(size / 4);
    for (int i = 0; i < rep; i++) {
        rand_fill(std::span(content_buff));
        memcpy(buff.virt_addr, &content_buff[0], size);
        buff.prepare();
        auto crc_dma = buff.crc32c_dma(p);
        auto crc_sw = crc32c(0, (const char*)&content_buff[0], size);
        if (crc_dma != crc_sw)
            printf("%d: %x, %x\n", i, crc_dma, crc_sw);
        // assert(crc_dma == crc_sw);
    }
}

int main()
{
    auto addr = Molecube::Pulser::address();
    Molecube::Pulser p(addr);

    printf("\n");
    printf("DMA read throughput\n");
    printf("HP DDR\n");
    bench_dma_only<DMAType::HP_DDR>(p, 4, 16 * 4096, 1000);
    printf("HP DDR WC\n");
    bench_dma_only<DMAType::HP_DDR_WC>(p, 4, 16 * 4096, 1000);
    printf("HP OCM\n");
    bench_dma_only<DMAType::HP_OCM>(p, 4, 16 * 4096, 1000);
    printf("HP OCM WC\n");
    bench_dma_only<DMAType::HP_OCM_WC>(p, 4, 16 * 4096, 1000);

    printf("ACP DDR\n");
    bench_dma_only<DMAType::ACP_DDR>(p, 4, 16 * 4096, 1000);
    printf("ACP OCM\n");
    bench_dma_only<DMAType::ACP_OCM>(p, 4, 16 * 4096, 1000);
    printf("ACP OCM WC\n");
    bench_dma_only<DMAType::ACP_OCM_WC>(p, 4, 16 * 4096, 1000);

    printf("ACP COH DDR\n");
    bench_dma_only<DMAType::ACP_COH_DDR>(p, 4, 16 * 4096, 1000);
    printf("ACP COH OCM\n");
    bench_dma_only<DMAType::ACP_COH_OCM>(p, 4, 16 * 4096, 1000);
    printf("ACP COH OCM WC\n");
    bench_dma_only<DMAType::ACP_COH_OCM_WC>(p, 4, 16 * 4096, 1000);

    printf("\n");
    printf("DMA pipe throughput\n");
    printf("HP DDR\n");
    bench_pipe<DMAType::HP_DDR>(p, 4, 16 * 4096, 1000);
    printf("HP DDR WC\n");
    bench_pipe<DMAType::HP_DDR_WC>(p, 4, 16 * 4096, 1000);
    printf("HP OCM\n");
    bench_pipe<DMAType::HP_OCM>(p, 4, 16 * 4096, 1000);
    printf("HP OCM WC\n");
    bench_pipe<DMAType::HP_OCM_WC>(p, 4, 16 * 4096, 1000);

    printf("ACP DDR\n");
    bench_pipe<DMAType::ACP_DDR>(p, 4, 16 * 4096, 1000);
    printf("ACP OCM\n");
    bench_pipe<DMAType::ACP_OCM>(p, 4, 16 * 4096, 1000);
    printf("ACP OCM WC\n");
    bench_pipe<DMAType::ACP_OCM_WC>(p, 4, 16 * 4096, 1000);

    printf("ACP COH DDR\n");
    bench_pipe<DMAType::ACP_COH_DDR>(p, 4, 16 * 4096, 1000);
    printf("ACP COH OCM\n");
    bench_pipe<DMAType::ACP_COH_OCM>(p, 4, 16 * 4096, 1000);
    printf("ACP COH OCM WC\n");
    bench_pipe<DMAType::ACP_COH_OCM_WC>(p, 4, 16 * 4096, 1000);

    printf("\n");
    printf("rand fill\n");
    printf("DDR\n");
    bench_rand_fill<DMAType::HP_DDR>(4, 16 * 4096, 1000);
    printf("DDR WC\n");
    bench_rand_fill<DMAType::HP_DDR_WC>(4, 16 * 4096, 1000);
    printf("OCM\n");
    bench_rand_fill<DMAType::HP_OCM>(4, 16 * 4096, 1000);
    printf("OCM WC\n");
    bench_rand_fill<DMAType::HP_OCM_WC>(4, 16 * 4096, 1000);

    printf("\n");
    printf("memcpy\n");
    printf("DDR\n");
    bench_memcpy<DMAType::HP_DDR>(4, 16 * 4096, 1000);
    printf("DDR WC\n");
    bench_memcpy<DMAType::HP_DDR_WC>(4, 16 * 4096, 1000);
    printf("OCM\n");
    bench_memcpy<DMAType::HP_OCM>(4, 16 * 4096, 1000);
    printf("OCM WC\n");
    bench_memcpy<DMAType::HP_OCM_WC>(4, 16 * 4096, 1000);

    printf("\n");
    printf("flush\n");
    printf("HP DDR\n");
    bench_flush<DMAType::HP_DDR>(4, 16 * 4096, 1000);
    printf("HP DDR WC\n");
    bench_flush<DMAType::HP_DDR_WC>(4, 16 * 4096, 1000);
    printf("HP OCM\n");
    bench_flush<DMAType::HP_OCM>(4, 16 * 4096, 1000);
    printf("HP OCM WC\n");
    bench_flush<DMAType::HP_OCM_WC>(4, 16 * 4096, 1000);

    printf("ACP DDR\n");
    bench_flush<DMAType::ACP_DDR>(4, 16 * 4096, 1000);
    printf("ACP OCM\n");
    bench_flush<DMAType::ACP_OCM>(4, 16 * 4096, 1000);
    printf("ACP OCM WC\n");
    bench_flush<DMAType::ACP_OCM_WC>(4, 16 * 4096, 1000);

    printf("ACP COH DDR\n");
    bench_flush<DMAType::ACP_COH_DDR>(4, 16 * 4096, 1000);
    printf("ACP COH OCM\n");
    bench_flush<DMAType::ACP_COH_OCM>(4, 16 * 4096, 1000);
    printf("ACP COH OCM WC\n");
    bench_flush<DMAType::ACP_COH_OCM_WC>(4, 16 * 4096, 1000);

    printf("\n");
    printf("Test crc32c\n");
    printf("HP DDR\n");
    test_crc32c<DMAType::HP_DDR>(p, 16 * 4096, 10000);
    printf("HP DDR WC\n");
    test_crc32c<DMAType::HP_DDR_WC>(p, 16 * 4096, 10000);
    printf("HP OCM\n");
    test_crc32c<DMAType::HP_OCM>(p, 16 * 4096, 10000);
    printf("HP OCM WC\n");
    test_crc32c<DMAType::HP_OCM_WC>(p, 16 * 4096, 10000);

    printf("ACP DDR\n");
    test_crc32c<DMAType::ACP_DDR>(p, 16 * 4096, 10000);
    printf("ACP OCM\n");
    test_crc32c<DMAType::ACP_OCM>(p, 16 * 4096, 10000);
    printf("ACP OCM WC\n");
    test_crc32c<DMAType::ACP_OCM_WC>(p, 16 * 4096, 10000);

    printf("ACP COH DDR\n");
    test_crc32c<DMAType::ACP_COH_DDR>(p, 16 * 4096, 10000);
    printf("ACP COH OCM\n");
    test_crc32c<DMAType::ACP_COH_OCM>(p, 16 * 4096, 10000);
    printf("ACP COH OCM WC\n");
    test_crc32c<DMAType::ACP_COH_OCM_WC>(p, 16 * 4096, 10000);

    dma_dbg_print(p, "final");
    return 0;
}
