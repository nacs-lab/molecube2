#include "../lib/pulser.h"
#include "../lib/dummy_pulser.h"

#include <chrono>
#include <stdio.h>
#include <thread>
#include <math.h>
#include <bitset>

static constexpr double pow2_32 = 1./pow(2, 32);

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

void print_binary(uint32_t n)
{
    while (n) {
        if (n & 1)
            printf("1");
        else
            printf("0");

        n>>= 1;
    }
    printf(" (highest order bit is ont he right)");
    printf("\n");
}

template<typename P>
void test_DDS_ramp(P &p){

    using namespace std::literals;

    int chn = 11;
    double f = 80e6;
    double amp = 0.1;

    if(!p.dds_exists(chn)){
        fprintf(stderr, "DDS channel %i does not exist", chn);
        return;
    }

    //-----------
    //Ramp from amplitude 1 to 0
    //-----------

    // First set DDS ouput to some frequency with full amplitude
    // Change DDS values
    p.template dds_set_freq<true>(chn, freq2num(f));
    p.template dds_set_amp<true>(chn, amp2num(amp));

    // Get the final value of DDS channe
    p.template dds_get_freq<true>(chn);
    double f_i = num2freq(p.get_result());
    p.template dds_get_amp<true>(chn);
    double amp_i = num2amp(p.get_result());

    printf("For DDS channel %i we start with freqnecy is %f and the amplitude is %f. \n", chn, f_i, amp_i);

    // Set registers
    //-----------
    // Set lower ramp limit register
    uint32_t reg_ramp_low_limit = 4;
    uint32_t ramp_low_limit = 0xFFFFFFFE;
    p.template dds_set_4bytes<true>(chn, reg_ramp_low_limit, ramp_low_limit);

    // Set upper ramp limit registe
    uint32_t reg_ramp_up_limit = 5;
    uint32_t ramp_up_limit = 0xFFFFFFFF;
    p.template dds_set_4bytes<true>(chn, reg_ramp_up_limit, ramp_up_limit);

    // Set ramp rate
    uint32_t reg_ramp_rate = 8;
    uint32_t ramp_rate = 0x00080008;
    p.template dds_set_4bytes<true>(chn, reg_ramp_rate, ramp_rate);

    // Set falling step size
    uint32_t reg_step_size = 7;
    uint32_t step_size = 0x08080808;
    p.template dds_set_4bytes<true>(chn, reg_step_size, step_size);

    // Set CFR2[21:19] to bits 111 (ramp amplitude and enable ramp)
    uint32_t reg_CFR2 = 1;
    p.template dds_get_4bytes<true>(chn, reg_CFR2);
    uint32_t CFR2_temp = p.get_result();
    printf("Just checkin %u \n", CFR2_temp);
    print_binary(CFR2_temp);
    uint32_t CFR2_data = CFR2_temp | 3670016;
    print_binary(CFR2_data);
    p.template dds_set_4bytes<true>(chn, reg_CFR2, CFR2_data);

    p.template dds_get_4bytes<true>(chn, reg_CFR2);
    uint32_t CFR2_temp2 = p.get_result();
    printf("Just checkin again %u \n", CFR2_temp2);
    print_binary(CFR2_temp2);

    // I/O update
}

int main()
{
    if (auto addr = Molecube::Pulser::address()) {
        Molecube::Pulser p(addr);
        test_DDS_ramp(p);
    }
    else {
        fprintf(stderr, "Pulse not enabled!\n");
    }

    //Molecube::DummyPulser dp;
    //test_DDS(dp);

    return 0;
}
