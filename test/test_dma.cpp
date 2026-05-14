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

#include <yaml-cpp/yaml.h>

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

enum class BuffType : uint8_t {
    DDR,
    DDR_WC,
    OCM,
    OCM_WC,
};

static inline const char *type_name(BuffType type)
{
    switch (type) {
    default:
        assert(false);
        return "";
    case BuffType::DDR:
        return "DDR";
    case BuffType::DDR_WC:
        return "DDR_WC";
    case BuffType::OCM:
        return "OCM";
    case BuffType::OCM_WC:
        return "OCM_WC";
    }
}

enum class DMAType : uint8_t {
    HP,
    ACP,
    ACP_L2,
    ACP_L1,
};

static inline const char *type_name(DMAType type)
{
    switch (type) {
    default:
        assert(false);
        return "";
    case DMAType::HP:
        return "HP";
    case DMAType::ACP:
        return "ACP";
    case DMAType::ACP_L2:
        return "ACP_L2";
    case DMAType::ACP_L1:
        return "ACP_L1";
    }
}

static inline void print_type(auto type)
{
    printf(" %6s", type_name(type));
}

static inline void print_type(BuffType buff_type, DMAType dma_type)
{
    printf(" %6s %6s", type_name(dma_type), type_name(buff_type));
}

template<BuffType buff_type, DMAType dma_type>
struct DMABuff {
    void *virt_addr;
    void *phy_addr;
    size_t size;

    static void *alloc_buff(size_t size)
    {
        switch (buff_type) {
        default:
            assert(false);
        case BuffType::DDR:
            return Kernel::allocDMABuffer(size, false);
        case BuffType::DDR_WC:
            return Kernel::allocDMABuffer(size, true);
        case BuffType::OCM:
            return Kernel::allocOCMBuffer(size, false);
        case BuffType::OCM_WC:
            return Kernel::allocOCMBuffer(size, true);
        }
    }

    friend void swap(DMABuff &b1, DMABuff &b2)
    {
        std::swap(b1.virt_addr, b2.virt_addr);
        std::swap(b1.phy_addr, b2.phy_addr);
        std::swap(b1.size, b2.size);
    }

    DMABuff(size_t size)
        : virt_addr(alloc_buff(size)),
          phy_addr(Kernel::bufferPhyAddr(virt_addr)),
          size(size)
    {}
    DMABuff()
        : virt_addr(nullptr),
          phy_addr(nullptr),
          size(0)
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
        switch (buff_type) {
        default:
            assert(false);
        case BuffType::DDR:
        case BuffType::DDR_WC:
            Kernel::freeDMABuffer(virt_addr, size);
            return;
        case BuffType::OCM:
        case BuffType::OCM_WC:
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
        switch (dma_type) {
        default:
            assert(false);
        case DMAType::HP:
            return _queue(p, 0x20, 0);
        case DMAType::ACP:
            return _queue(p, 0x21, 0);
        case DMAType::ACP_L2:
            return _queue(p, 0x21, 2);
        case DMAType::ACP_L1:
            return _queue(p, 0x21, 3);
        }
    }

    void prepare() const
    {
        if (buff_type == BuffType::OCM_WC || buff_type == BuffType::DDR_WC ||
            dma_type == DMAType::ACP_L1) {
            asm volatile ("dmb st" ::: "memory");
            return;
        }
        if (buff_type == BuffType::OCM || dma_type == DMAType::ACP_L2)
            return Kernel::cleanCache(virt_addr, size, true);
        return Kernel::cleanCache(virt_addr, size, false);
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
        if (dma_type == DMAType::HP)
            return p.read(0x34);
        return p.read(0x35);
    }
};

static double bench_rep(auto &&cb, int rep, int div, auto &&finish)
{
    Timer timer;
    for (int i = 0; i < rep; i++) {
        cb();
    }
    finish();
    auto dt = double(timer.elapsed()) / rep / div;
    auto dt_origin = dt;
    if (dt <= 1300) {
        printf(": % 8.3f ns\n", dt);
        return dt_origin;
    }
    dt /= 1000;
    if (dt <= 1300) {
        printf(": % 8.3f us\n", dt);
        return dt_origin;
    }
    dt /= 1000;
    if (dt <= 1300) {
        printf(": % 8.3f ms\n", dt);
        return dt_origin;
    }
    dt /= 1000;
    printf(": % 8.3f s\n", dt);
    return dt_origin;
}

template<template<BuffType buff_type> typename Runner, typename ... Args>
static void run_buff(YAML::Emitter &yaml, Args&&... args)
{
    yaml << YAML::BeginMap;
    yaml << YAML::Key << "DDR" << YAML::Value;
    Runner<BuffType::DDR>::run(yaml, args...);
    yaml << YAML::Key << "DDR_WC" << YAML::Value;
    Runner<BuffType::DDR_WC>::run(yaml, args...);
    yaml << YAML::Key << "OCM" << YAML::Value;
    Runner<BuffType::OCM>::run(yaml, args...);
    yaml << YAML::Key << "OCM_WC" << YAML::Value;
    Runner<BuffType::OCM_WC>::run(yaml, args...);
    yaml << YAML::EndMap;
}

template<template<BuffType buff_type, DMAType dma_type> typename Runner, typename ... Args>
static void run_dma_buff(YAML::Emitter &yaml, Args&&... args)
{
    yaml << YAML::BeginMap;

    yaml << YAML::Key << "HP" << YAML::Value;
    yaml << YAML::BeginMap;
    yaml << YAML::Key << "DDR" << YAML::Value;
    Runner<BuffType::DDR,DMAType::HP>::run(yaml, args...);
    yaml << YAML::Key << "DDR_WC" << YAML::Value;
    Runner<BuffType::DDR_WC,DMAType::HP>::run(yaml, args...);
    yaml << YAML::Key << "OCM" << YAML::Value;
    Runner<BuffType::OCM,DMAType::HP>::run(yaml, args...);
    yaml << YAML::Key << "OCM_WC" << YAML::Value;
    Runner<BuffType::OCM_WC,DMAType::HP>::run(yaml, args...);
    yaml << YAML::EndMap;

    yaml << YAML::Key << "ACP" << YAML::Value;
    yaml << YAML::BeginMap;
    yaml << YAML::Key << "DDR" << YAML::Value;
    Runner<BuffType::DDR,DMAType::ACP>::run(yaml, args...);
    yaml << YAML::Key << "DDR_WC" << YAML::Value;
    Runner<BuffType::DDR_WC,DMAType::ACP>::run(yaml, args...);
    yaml << YAML::Key << "OCM" << YAML::Value;
    Runner<BuffType::OCM,DMAType::ACP>::run(yaml, args...);
    yaml << YAML::Key << "OCM_WC" << YAML::Value;
    Runner<BuffType::OCM_WC,DMAType::ACP>::run(yaml, args...);
    yaml << YAML::EndMap;

    yaml << YAML::Key << "ACP_L2" << YAML::Value;
    yaml << YAML::BeginMap;
    yaml << YAML::Key << "DDR" << YAML::Value;
    Runner<BuffType::DDR,DMAType::ACP_L2>::run(yaml, args...);
    yaml << YAML::Key << "DDR_WC" << YAML::Value;
    Runner<BuffType::DDR_WC,DMAType::ACP_L2>::run(yaml, args...);
    yaml << YAML::Key << "OCM" << YAML::Value;
    Runner<BuffType::OCM,DMAType::ACP_L2>::run(yaml, args...);
    yaml << YAML::Key << "OCM_WC" << YAML::Value;
    Runner<BuffType::OCM_WC,DMAType::ACP_L2>::run(yaml, args...);
    yaml << YAML::EndMap;

    yaml << YAML::Key << "ACP_L1" << YAML::Value;
    yaml << YAML::BeginMap;
    yaml << YAML::Key << "DDR" << YAML::Value;
    Runner<BuffType::DDR,DMAType::ACP_L1>::run(yaml, args...);
    yaml << YAML::Key << "DDR_WC" << YAML::Value;
    Runner<BuffType::DDR_WC,DMAType::ACP_L1>::run(yaml, args...);
    yaml << YAML::Key << "OCM" << YAML::Value;
    Runner<BuffType::OCM,DMAType::ACP_L1>::run(yaml, args...);
    yaml << YAML::Key << "OCM_WC" << YAML::Value;
    Runner<BuffType::OCM_WC,DMAType::ACP_L1>::run(yaml, args...);
    yaml << YAML::EndMap;

    yaml << YAML::EndMap;
}

template<BuffType buff_type, DMAType dma_type>
struct BenchDMAOnly {
    static void run(YAML::Emitter &yaml, Molecube::Pulser &p, int nbuffs, size_t size, int rep)
    {
        print_type(buff_type, dma_type);
        std::vector<DMABuff<buff_type,dma_type>> buffs(nbuffs);
        for (int i = 0; i < nbuffs; i++) {
            buffs[i] = DMABuff<buff_type,dma_type>(size);
        }
        auto start_count = p.read(0x31);
        int j = 0;
        auto last_count = start_count;
        yaml << bench_rep([&] {
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
};

template<BuffType buff_type, DMAType dma_type>
struct BenchDMARW {
    static void run(YAML::Emitter &yaml, Molecube::Pulser &p, int nbuffs, size_t size, int rep)
    {
        print_type(buff_type, dma_type);
        std::vector<DMABuff<buff_type,dma_type>> buffs(nbuffs);
        for (int i = 0; i < nbuffs; i++) {
            buffs[i] = DMABuff<buff_type,dma_type>(size);
        }
        auto max_ahead = nbuffs;
        if (max_ahead > 8)
            max_ahead = 8;
        std::vector<uint32_t> content_buff(size / 4);
        rand_fill(std::span(content_buff));
        auto start_count = p.read(0x31);
        int j = 0;
        auto last_count = start_count;
        yaml << bench_rep([&] {
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
};

template<BuffType buff_type>
struct BenchRandFill {
    static void run(YAML::Emitter &yaml, size_t size, int rep)
    {
        print_type(buff_type);
        DMABuff<buff_type,DMAType::HP> buff(size);
        yaml << bench_rep([&] {
            buff.rand_fill();
            asm volatile ("" ::: "memory");
        }, rep, 1, [] {});
    }
};

template<BuffType buff_type>
struct BenchMemcpy {
    static void run(YAML::Emitter &yaml, size_t size, int rep)
    {
        print_type(buff_type);
        DMABuff<buff_type,DMAType::HP> buff(size);
        std::vector<uint32_t> content_buff(size / 4);
        yaml << bench_rep([&] {
            memcpy(buff.virt_addr, &content_buff[0], size);
            asm volatile ("" ::: "memory");
        }, rep, 1, [] {});
    }
};

template<BuffType buff_type, DMAType dma_type>
struct BenchFlush {
    static void run(YAML::Emitter &yaml, size_t size, int rep)
    {
        print_type(buff_type, dma_type);
        DMABuff<buff_type,dma_type> buff(size);
        yaml << bench_rep([&] {
            buff.prepare();
        }, rep, 1, [] {});
    }
};

template<BuffType buff_type, DMAType dma_type>
struct TestCRC32c {
    static void run(YAML::Emitter &yaml, Molecube::Pulser &p, size_t size, int rep)
    {
        print_type(buff_type, dma_type);
        printf("\n");
        DMABuff<buff_type,dma_type> buff(size);
        std::vector<uint32_t> content_buff(size / 4);
        int failed = 0;
        for (int i = 0; i < rep; i++) {
            rand_fill(std::span(content_buff));
            memcpy(buff.virt_addr, &content_buff[0], size);
            buff.prepare();
            auto crc_dma = buff.crc32c_dma(p);
            auto crc_sw = crc32c(0, (const char*)&content_buff[0], size);
            if (crc_dma != crc_sw) {
                if (failed == 0)
                    printf("  First failed@%d: %08x != %08x\n", i, crc_dma, crc_sw);
                failed += 1;
            }
        }
        if (failed != 0) {
            printf("  Total failed: %d/%d\n", failed, rep);
        }
        yaml << double(failed) / rep;
    }
};

static void bench_all_nbuff(YAML::Emitter &yaml, Molecube::Pulser &p,
                            int nbuff, size_t size, int rep)
{
    yaml << YAML::BeginMap;
    yaml << YAML::Key << "nbuff" << YAML::Value << nbuff;
    yaml << YAML::Key << "size" << YAML::Value << size;
    yaml << YAML::Key << "rep" << YAML::Value << rep;
    yaml << YAML::Key << "results" << YAML::Value;

    yaml << YAML::BeginMap;

    yaml << YAML::Key << "dma_only" << YAML::Value;
    printf("DMA read throughput %d x [%zu] (rep: %d)\n", nbuff, size, rep);
    run_dma_buff<BenchDMAOnly>(yaml, p, nbuff, size, rep);
    printf("\n");

    yaml << YAML::Key << "dma_pipe" << YAML::Value;
    printf("DMA read write %d x [%zu] (rep: %d)\n", nbuff, size, rep);
    run_dma_buff<BenchDMARW>(yaml, p, nbuff, size, rep);
    printf("\n");

    yaml << YAML::EndMap;

    yaml << YAML::EndMap;
}

static void bench_all_buff1(YAML::Emitter &yaml, Molecube::Pulser &p,
                            size_t size, int rep)
{
    yaml << YAML::BeginMap;
    yaml << YAML::Key << "size" << YAML::Value << size;
    yaml << YAML::Key << "rep" << YAML::Value << rep;
    yaml << YAML::Key << "results" << YAML::Value;

    yaml << YAML::BeginMap;

    yaml << YAML::Key << "rand_fill" << YAML::Value;
    printf("rand fill [%zu] (rep: %d)\n", size, rep);
    run_buff<BenchRandFill>(yaml, size, rep);
    printf("\n");

    yaml << YAML::Key << "memcpy" << YAML::Value;
    printf("memcpy [%zu] (rep: %d)\n", size, rep);
    run_buff<BenchMemcpy>(yaml, size, rep);
    printf("\n");

    yaml << YAML::Key << "flush" << YAML::Value;
    printf("flush [%zu] (rep: %d)\n", size, rep);
    run_dma_buff<BenchFlush>(yaml, size, rep);
    printf("\n");

    yaml << YAML::Key << "crc32c" << YAML::Value;
    printf("crc32c [%zu] (rep: %d)\n", size, rep);
    run_dma_buff<TestCRC32c>(yaml, p, size, rep);
    printf("\n");

    yaml << YAML::EndMap;

    yaml << YAML::EndMap;
}

int main()
{
    auto addr = Molecube::Pulser::address();
    Molecube::Pulser p(addr);

    YAML::Emitter yaml;
    yaml << YAML::BeginSeq;
    for (size_t size: {16 * 8, 16 * 16, 16 * 32, 16 * 64, 16 * 128, 16 * 256,
            16 * 1024, 16 * 2048, 16 * 4096}) {
        for (int nbuff = 1; nbuff <= 4; nbuff++) {
            bench_all_nbuff(yaml, p, nbuff, size, 1000);
        }
        bench_all_buff1(yaml, p, size, 1000);
    }
    yaml << YAML::EndSeq;

    std::cout << yaml.c_str() << std::endl;

    dma_dbg_print(p, "final");
    return 0;
}
