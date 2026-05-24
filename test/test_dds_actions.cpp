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
    p.template dds_set_freq<false>(chn, freq2num(10e6 * type));
    p.template dds_set_amp<false>(chn, amp2num(0.1 * type));
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

    return 0;
}
