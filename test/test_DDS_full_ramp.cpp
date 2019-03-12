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
    /*
    unsigned nb=0; // bit reversed n
    for(int i=0; i<32; i++)
    {
        unsigned mask = 1 << i;
        if(n & mask)
            nb += 1 << (31-i);
    }

    printf("0x%08X\n0x%08X\n", n,nb);
    */
    for (int i = 0; i < size; i++) {
        printf("%i", n%2);
        n /= 2;
    }
    printf(" (highest order bit is on the right)");
    printf("\n");
}

// Declare the first byte address for different registries
uint32_t reg_CFR1 = 0x00;
uint32_t reg_CFR2 = 0x04;
uint32_t reg_rampLowLimit = 0x10;
uint32_t reg_rampHighLimit = 0x14;
uint32_t reg_riseStep = 0x18;
uint32_t reg_fallStep = 0x1C;
uint32_t reg_rampRate = 0x20;

// The following global variable should probably be part of the pulser object
uint32_t amplitude = 0; // NOTE: make sure to add some dependency on channel
// NOTE: amplitude could also always be stored in the amplitude registry of the specific DDS channel

template<typename P>
void setRampSpeed(P &p, int chn, uint32_t ramp_rate = 0x00080008, uint32_t step_down = 0x1, uint32_t step_up = 0x1)
{
    using namespace std::literals;

    p.template dds_set_4bytes<true>(chn, reg_rampRate, ramp_rate);
    p.template dds_set_4bytes<true>(chn, reg_riseStep, step_up);
    p.template dds_set_4bytes<true>(chn, reg_fallStep, step_down);
}

template<typename P>
void setRampLimits(P &p, int chn, uint32_t lowLimit = 0x0, uint32_t highLimit = 0xFFF)
{
    using namespace std::literals;

    p.template dds_set_4bytes<true>(chn, reg_rampLowLimit, lowLimit);
    p.template dds_set_4bytes<true>(chn, reg_rampHighLimit, highLimit);
}

template<typename P>
void rampAmpTo(P &p, int chn, double amp_final, uint32_t ramp_rate = 0x00080008, uint32_t step_size = 0x1)
{
    using namespace std::literals;

    //NOTE: The commented our part would be okay if we always store the amplitude into the amplitude register (0x0C)
    // First get initial amplitude and set direction of ramp
    //p.template dds_get_amp<true>(chn);
    //uint32_t amp_init = p.get_result();

    bool up = 0;
    if(num2amp(amplitude) < amp_final) up = 1;

    // Once the ramp bit is enabled the output JUMPS to the lower limit
    // so the lower limit should be set to the current amplitude regardless of the direction of ramping

    // Set ramp speed (and direction by setting the step size)
    if(up)
        setRampSpeed(p, chn, ramp_rate, 0x0, step_size);
    else
        setRampSpeed(p, chn, ramp_rate, step_size, 0x0);

     // Initiate the ramp:
    // Set CFR2[21:19] to bits 111 (ramp amplitude and enable ramp)
    // Also set CFR2[17:18] bits to 11 (no-dwell high and low) -> oscilate between limits
    p.template dds_get_4bytes<true>(chn, reg_CFR2);
    uint32_t temp = p.get_result();
    uint32_t CFR2_data = temp | 0b111110 << 16;
    p.template dds_set_4bytes<true>(chn, reg_CFR2, CFR2_data);

    if(up)
        setRampLimits(p, chn, amplitude, amp2num(amp_final));
    else
        setRampLimits(p, chn, amp2num(amp_final), amplitude);

    amplitude = amp2num(amp_final);
}

template<typename P>
void test_DDS_ramp(P &p)
{
    using namespace std::literals;

    int chn = 1;
    double f = 1e6;
    double amp = 0.0;

    amplitude = amp2num(amp);

    // Initialize DDS channel
    p.init_dds(chn);

    if(!p.dds_exists(chn)){
        fprintf(stderr, "DDS channel %i does not exist", chn);
        return;
    }

    // First set DDS ouput to some frequency with full amplitude
    // Change DDS values
    p.template dds_set_freq<true>(chn, freq2num(f));
    p.template dds_set_amp<true>(chn, amp2num(amp));

    // Get the initial value of DDS channel and print it out
    p.template dds_get_freq<true>(chn);
    uint32_t freq_i = p.get_result();
    p.template dds_get_amp<true>(chn);
    uint32_t amp_i = p.get_result();

    printf("The initial frequency and amplitude of channel %i are %f MHz and %f V. \n", chn, num2freq(freq_i), num2amp(amp_i));

    std::this_thread::sleep_for(1000ms);

    //Set CFR1[8:9] bits to 11 (OSK enable)
    p.template dds_get_4bytes<true>(chn, reg_CFR1);
    uint32_t temp = p.get_result();
    temp = temp | 0b1100000000;
    p.template dds_set_4bytes<true>(chn, reg_CFR1, temp);

    rampAmpTo(p, chn, 0.9, 0x00080008, 0x1);
    std::this_thread::sleep_for(200us);
    rampAmpTo(p, chn, 0.3, 0x00080008, 0x3);
    std::this_thread::sleep_for(100us);

    // NOTE: the rampAmpTo function only sets the bits for the ramp, it doesn't actually "pause" the thread until the ramp is over
    // Thus the sleep thread part afterwards should be sufficiently long such that the ramp is done
    // Will have to address this in a better way in the future

    // NOTE: I haven't properly looked into the ramp rate register yet and which precise bits are actually relevant.
    // Will have to address this as well
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
