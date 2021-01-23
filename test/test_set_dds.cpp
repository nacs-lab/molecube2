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
#include <math.h>

constexpr double pow2_32 = 1. / pow(2, 32);

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

int main(int argc, char **argv)
{
    if (argc != 4) {
        printf("Require three arguments: test_set_dds chn freq amp\n");
        printf("  Use a negative number for freq or amp to skip setting the value.\n");
        return 1;
    }
    int chn = atoi(argv[1]);
    if (chn < 0 || chn >= 22) {
        printf("Channel number %d out of range.\n", chn);
        return 1;
    }

    double freq = atof(argv[2]);
    double amp = atof(argv[3]);

    auto addr = Molecube::Pulser::address();
    if (!addr) {
        printf("Cannot find pulser\n");
        return 1;
    }

    Molecube::Pulser p(addr);
    if (freq >= 0) {
        auto set_freq = freq2num(freq);
        p.dds_set_freq<false>(chn, set_freq);
        p.dds_get_freq<false>(chn);
        auto new_freq = p.get_result();
        printf("New frequency: %f\n", num2freq(new_freq));
        assert(new_freq == set_freq);
    }
    else {
        p.dds_get_freq<false>(chn);
        printf("Frequency: %f\n", num2freq(p.get_result()));
    }
    if (amp >= 0) {
        auto set_amp = amp2num(amp);
        p.dds_set_amp<false>(chn, set_amp);
        p.dds_get_amp<false>(chn);
        auto new_amp = p.get_result();
        printf("New amplitude: %f\n", num2amp(new_amp));
        assert(new_amp == set_amp);
    }
    else {
        p.dds_get_amp<false>(chn);
        printf("Amplitude: %f\n", num2amp(p.get_result()));
    }
    return 0;
}
