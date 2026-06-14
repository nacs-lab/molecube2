#include "../lib/pulser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <iostream>

static constexpr double pow2_32 = 1. / pow(2, 32);

static inline constexpr uint32_t
freq2num(double f, double clock = 3.5e9)
{
    return static_cast<uint32_t>(0.5 + f / clock * (1 / pow2_32));
}

static inline constexpr double
num2freq(uint32_t num, double clock = 3.5e9)
{
    return num * clock * pow2_32;
}

static inline constexpr uint16_t
amp2num(double amp)
{
    return 0x0fff & static_cast<uint16_t>(amp * 4095.0 + 0.5);
}

static inline constexpr double
num2amp(uint32_t num)
{
    return num / 4095.0;
}

static inline void
set_dds_wr_timing(Molecube::Pulser &p, uint32_t adsu, uint32_t wrlow, uint32_t adhd,
                  uint32_t fuddl, uint32_t fudhd)
{
    p.write(0x50, adsu | (wrlow << 6) | (adhd << 12) | (fuddl << 18) | (fudhd << 24));
}

static uint32_t dds_loopback_data(int i, uint32_t addr, uint32_t type)
{
    return (addr + type) | (addr << 8) | ((addr + i + type) << 16) |
        ((addr + 3 - i / 2 + type) << 24);
}

static bool check_dds_loopback_type(Molecube::Pulser &p, int i, uint32_t type)
{
    for (uint32_t addr = 0x34; addr < 0x6c; addr += 4) {
        if (addr == 0x64)
            continue;
        p.dds_set_4bytes<false>(i, addr, dds_loopback_data(i, addr, type));
        p.dds_get_4bytes<false>(i, addr);
    }
    bool pass = true;
    for (uint32_t addr = 0x34; addr < 0x6c; addr += 4) {
        if (addr == 0x64)
            continue;
        if (p.get_result() != dds_loopback_data(i, addr, type)) {
            pass = false;
        }
    }
    return pass;
}

static bool check_dds_loopback(Molecube::Pulser &p, int i)
{
    return (check_dds_loopback_type(p, i, 0) && check_dds_loopback_type(p, i, 1) &&
            check_dds_loopback_type(p, i, 2) && check_dds_loopback_type(p, i, 3));
}

static int check_all_dds_loopback(Molecube::Pulser &p, const std::vector<int> &ids)
{
    int fail_count = 0;
    for (int i: ids) {
        if (!check_dds_loopback(p, i)) {
            fail_count += 1;
        }
    }
    return fail_count;
}

static void scan_dds_timing(Molecube::Pulser &p, const std::vector<int> &ids)
{
    uint8_t timings[5] = {8, 8, 8, 8, 8};
    auto set_timing = [&] {
        set_dds_wr_timing(p, timings[0], timings[1], timings[2],
                          timings[3], timings[4]);
    };
    auto print_fail_count = [&] (int cnt) {
        printf("%d, %d, %d, %d, %d: fail=%d\n", timings[0], timings[1], timings[2],
               timings[3], timings[4], cnt);
    };
    uint8_t min_timings[5] = {};

    set_timing();
    print_fail_count(check_all_dds_loopback(p, ids));
    for (int tid = 0; tid < 5; tid++) {
        bool failed = false;
        for (int t = 7; t >= 0; t--) {
            timings[tid] = uint8_t(t);
            set_timing();
            auto cnt = check_all_dds_loopback(p, ids);
            print_fail_count(cnt);
            if (cnt > 0 && !failed) {
                failed = true;
                min_timings[tid] = uint8_t(t + 1);
            }
        }
        timings[tid] = 8;
    }
    memcpy(timings, min_timings, sizeof(timings));
    set_timing();
    print_fail_count(check_all_dds_loopback(p, ids));
}

static std::vector<int> parse_dds_ids(const char *s)
{
    std::vector<int> ids;
    int start = -1;
    while (*s) {
        char *endptr;
        auto v = (int)strtol(s, &endptr, 10);
        switch (*endptr) {
        case '\0':
        case ',':
            if (start >= 0) {
                for (int i = start; i < v; i++) {
                    ids.push_back(i);
                }
                start = -1;
            }
            ids.push_back(v);
            break;
        case '-':
            if (start >= 0) {
                fprintf(stderr, "Invalid DDS range\n");
            }
            start = v;
        }
        if (!*endptr)
            break;
        s = endptr + 1;
    }
    return ids;
}

void reset_dds(Molecube::Pulser &p, int i)
{
    p.dump_dds(std::cout, i);
    p.dds_reset<true>(i);
    while (!p.is_finished()) {
    }
    p.dump_dds(std::cout, i);
}

void write_dds(Molecube::Pulser &p, int i, int type)
{
    p.dump_dds(std::cout, i);
    for (int addr = 0x34; addr < 0x6c; addr += 2) {
        if (addr == 0x64 || addr == 0x66)
            continue;
        p.dds_set_2bytes<false>(i, addr, (addr + type) | (addr << 8));
    }
    while (!p.is_finished()) {
    }
    p.dump_dds(std::cout, i);
}

void output_dds(Molecube::Pulser &p, int i, int type)
{
    p.dump_dds(std::cout, i);
    p.template dds_set_freq<false>(i, freq2num(10e6 * type));
    p.template dds_set_amp<false>(i, amp2num(0.1 * type));
    while (!p.is_finished()) {
    }
    p.dump_dds(std::cout, i);
}

int main(int argc, char **argv)
{
    auto addr = Molecube::Pulser::address();
    if (!addr) {
        printf("Cannot find pulser\n");
        return 1;
    }
    Molecube::Pulser p(addr);
    if (argc != 3) {
        printf("Require two arguments: test_dds_actions action chn\n");
        return 1;
    }
    if (strcmp(argv[1], "reset") == 0) {
        reset_dds(p, atoi(argv[2]));
    }
    else if (strcmp(argv[1], "write1") == 0) {
        write_dds(p, atoi(argv[2]), 1);
    }
    else if (strcmp(argv[1], "write2") == 0) {
        write_dds(p, atoi(argv[2]), 2);
    }
    else if (strcmp(argv[1], "output1") == 0) {
        output_dds(p, atoi(argv[2]), 1);
    }
    else if (strcmp(argv[1], "output2") == 0) {
        output_dds(p, atoi(argv[2]), 2);
    }
    else if (strcmp(argv[1], "scan_timing") == 0) {
        scan_dds_timing(p, parse_dds_ids(argv[2]));
    }

    return 0;
}
