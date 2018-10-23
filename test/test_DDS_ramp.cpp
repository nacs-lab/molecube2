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

void print_binary(uint32_t n, int size = 32)
{
    unsigned nb=0; // bit reversed n
    for(int i=0; i<32; i++)
    {
        unsigned mask = 1 << i;
        if(n & mask)
            nb += 1 << (31-i);
    }

    printf("0x%08X\n0x%08X\n", n,nb);
    for (int i = 0; i < size; i++) {
        printf("%i", n%2);
        n /= 2;
    }
    printf(" (highest order bit is on the right)");
    printf("\n");
}

struct DDS_propr {
    int chn;
    double f;
    double amp;
};

template<typename P>
DDS_propr get_set_freq_amp (P &p, int channel)
{
    // NOTE:
    //------------
    // The get amp/freq functions only read whatever freq/amp was set.
    // They don't necessarily get the ACTUAL freq or amp.
    DDS_propr a;
    a.chn = channel;
    p.template dds_get_freq<true>(channel);
    a.f  = num2freq(p.get_result());
    p.template dds_get_amp<true>(channel);
    a.amp = num2amp(p.get_result());

    return a;
}

template<typename P>
void test_DDS_ramp(P &p){

    using namespace std::literals;

    int chn = 1;
    double f = 1e6;
    double amp = 1.0;

    // Initialize DDS channel
    p.init_dds(chn);

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

    // Get the initial value of DDS channe

    DDS_propr dds_i = get_set_freq_amp (p, chn);
    printf("For DDS channel %i we start with freqnecy is %f and the amplitude is %f. \n", dds_i.chn, dds_i.f, dds_i.amp);

    std::this_thread::sleep_for(1000ms);

    // Set registers
    //-----------
    // Set upper ramp limit register
    uint32_t reg_ramp_up_limit = 0x14;
    uint32_t ramp_up_limit = 0xFFF;//0b100000000000 << 20;
    p.template dds_get_4bytes<true>(chn, reg_ramp_up_limit);
    uint32_t ramp_up_limit_temp = p.get_result();
    printf("The ramp up limit registry was initially set to  ");
    print_binary(ramp_up_limit_temp);
    p.template dds_set_4bytes<true>(chn, reg_ramp_up_limit, ramp_up_limit);
    p.template dds_get_4bytes<true>(chn, reg_ramp_up_limit);
    ramp_up_limit_temp = p.get_result();
    printf("The ramp up limit registry is set to             ");
    print_binary(ramp_up_limit_temp);

    // Set lower ramp limit register
    uint32_t reg_ramp_low_limit = 0x10;
    uint32_t ramp_low_limit = 0x000;
    p.template dds_get_4bytes<true>(chn, reg_ramp_low_limit);
    uint32_t ramp_low_limit_temp = p.get_result();
    printf("The ramp low limit registry was initially set to ");
    print_binary(ramp_low_limit_temp);
    p.template dds_set_4bytes<true>(chn, reg_ramp_low_limit, ramp_low_limit);
    p.template dds_get_4bytes<true>(chn, reg_ramp_low_limit);
    ramp_low_limit_temp = p.get_result();
    printf("The ramp low limit registry is set to            ");
    print_binary(ramp_low_limit_temp);

    std::this_thread::sleep_for(1000ms);

    //Set ramp rate
    uint32_t reg_ramp_rate = 0x20;
    uint32_t ramp_rate = 0x00080008;//0x80018001; //
    p.template dds_set_4bytes<true>(chn, reg_ramp_rate, ramp_rate);

    std::this_thread::sleep_for(1000ms);

    // Set rising step size
    int reg_rise_step_size = 0x18;
    p.template dds_get_4bytes<true>(chn, reg_rise_step_size);
    printf("The rise step size was set to   ");
    uint32_t rise_step_size_temp = p.get_result();
    print_binary(rise_step_size_temp);
    uint32_t rise_step_size = 0x1;
    p.template dds_set_4bytes<true>(chn, reg_rise_step_size, rise_step_size);
    p.template dds_get_4bytes<true>(chn, reg_rise_step_size);
    rise_step_size_temp = p.get_result();
    printf("The rise step size is set to    ");
    print_binary(rise_step_size_temp);

    std::this_thread::sleep_for(1000ms);

    // Set falling step size
    uint32_t reg_fall_step_size = 0x1C;
    p.template dds_get_4bytes<true>(chn, reg_fall_step_size);
    printf("The fall step size was set to   ");
    uint32_t fall_step_size_temp = p.get_result();
    print_binary(fall_step_size_temp);
    uint32_t fall_step_size = 0x1;
    p.template dds_set_4bytes<true>(chn, reg_fall_step_size, fall_step_size);
    p.template dds_get_4bytes<true>(chn, reg_fall_step_size);
    fall_step_size_temp = p.get_result();
    printf("The fall step size is set to    ");
    print_binary(fall_step_size_temp);

    std::this_thread::sleep_for(1000ms);

    // Set CFR1[8:9] to bits 11 (OSK enable)
    uint32_t reg_CFR1 = 0x00;
    p.template dds_get_4bytes<true>(chn, reg_CFR1);
    uint32_t CFR1_temp = p.get_result();
    printf("CFR1 bits before and after \n");
    printf("CFR1 bits before: ");
    print_binary(CFR1_temp);
    uint32_t CFR1_data = CFR1_temp | 0b1100000000;
    p.template dds_set_4bytes<true>(chn, reg_CFR1, CFR1_data);
    p.template dds_get_4bytes<true>(chn, reg_CFR1);
    uint32_t CFR1_temp2 = p.get_result();
    printf("CFR1 bits after:  ");
    print_binary(CFR1_temp2);

    std::this_thread::sleep_for(1000ms);

    // Set CFR2[21:19] to bits 111 (ramp amplitude and enable ramp)
    // Also set CFR2[17:18] bits to 11 (no-dwell high and low) -> oscilate between limits
    uint32_t reg_CFR2 = 0x04;
    p.template dds_get_4bytes<true>(chn, reg_CFR2);
    uint32_t CFR2_temp = p.get_result();
    printf("CFR2 bits before and after \n");
    printf("CFR2 bits before: ");
    print_binary(CFR2_temp);
    uint32_t CFR2_data = CFR2_temp | 0b111110 << 16;//3670016; //524288;
    p.template dds_set_4bytes<true>(chn, reg_CFR2, CFR2_data);
    p.template dds_get_4bytes<true>(chn, reg_CFR2);
    uint32_t CFR2_temp2 = p.get_result();
    printf("CFR2 bits after:  ");
    print_binary(CFR2_temp2);

    std::this_thread::sleep_for(1000ms);

    // I/O update
    // don't need to, apparently

    // Reset at the end
    //p.template dds_reset<true>(chn);

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
