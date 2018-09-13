#include "../lib/pulser.h"
#include "../lib/dummy_pulser.h"

#include <chrono>
#include <stdio.h>
#include <thread>
#include <math.h>

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

template<typename P>
void test_DDS(P &p)
{
    using namespace std::literals;

    int chn = 11;
    double f = 80e6;
    double amp = 0.1;

    printf("hahaha %f hahaha %f \n", f, amp);

    // Check whether DDS channel is connected, initialize it
    // if (!p.check_dds(chn, true)) {
    //     fprintf(stderr, "DDS channel %i is not okay", chn);
    //     return;
    // }
    if (!p.dds_exists(chn)) {
        fprintf(stderr, "DDS channel %i does not exist", chn);
        return;
    }

    // Get initial value of DDS channel
    p.template dds_get_freq<true>(chn);
    uint32_t f_i_num = p.get_result();
    double f_i = num2freq(f_i_num);
    p.template dds_get_amp<true>(chn);
    uint32_t amp_i_num = p.get_result();
    double amp_i = num2amp(amp_i_num);

    printf("For DDS channel %i the current frequency is %f and the amplitude is %f.\n",
           chn, f_i, amp_i);

    // Change DDS values
    p.template dds_set_freq<true>(chn, freq2num(f));
    p.template dds_set_amp<true>(chn, amp2num(amp));

    // Get the final value of DDS channe
    p.template dds_get_freq<true>(chn);
    double f_f = num2freq(p.get_result());
    p.template dds_get_amp<true>(chn);
    double amp_f = num2amp(p.get_result());

    printf("For DDS channel %i the current freqnecy is %f and the amplitude is %f.\n",
           chn, f_f, amp_f);

    printf("Ok, bye now!\n");
}

int main()
{
    if (auto addr = Molecube::Pulser::address()) {
        Molecube::Pulser p(addr);
        test_DDS(p);
    }
    else {
        fprintf(stderr, "Pulse not enabled!\n");
    }

    // Molecube::DummyPulser dp;
    // test_DDS(dp);
    // test_DDS(dp);

    return 0;
}
