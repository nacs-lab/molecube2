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

#include "pulser.h"

#include <chrono>
#include <thread>

namespace Molecube {

bool Pulser::try_get_result(uint32_t &res)
{
    if (!num_results())
        return false;
    res = pop_result();
    return true;
}

uint32_t Pulser::get_result()
{
    // Used in cases where we don't care about the performance
    // of the calling thread too much.
    uint32_t res;
    while (!try_get_result(res))
        std::this_thread::yield();
    return res;
}

void Pulser::init_dds(int chn)
{
    using namespace std::literals;

    dds_reset<false>(chn);

    // calibrate internal timing.  required at power-up
    dds_set_2bytes<false>(chn, 0x0e, 0x0105);
    std::this_thread::sleep_for(1ms);
    // finish cal. disble sync_out
    dds_set_2bytes<false>(chn, 0x0e, 0x0405);

    // enable programmable modulus and profile 0, enable SYNC_CLK output
    // dds_set_2bytes<false>(chn, 0x05, 0x8d0b);

    // disable programmable modulus, enable profile 0,
    // enable SYNC_CLK output
    // dds_set_2bytes<false>(chn, 0x05, 0x8009);

    // disable ramp & programmable modulus, enable profile 0,
    // disable SYNC_CLK output
    // dds_set_2bytes<false>(chn, 0x05, 0x8001);

    // disable SYNC_CLK output
    dds_set_2bytes<false>(chn, 0x04, 0x0100);

    // enable ramp, enable programmable modulus, disable profile mode
    // dds_set_2bytes<false>(chn, 0x06, 0x0009);

    // disable ramp, disable programmable modulus, enable profile mode
    dds_set_2bytes<false>(chn, 0x06, 0x0080);

    // enable amplitude control (OSK)
    dds_set_2bytes<false>(chn, 0x0, 0x0308);

    // zero-out all other memory
    for (unsigned addr = 0x10;addr <= 0x6a;addr += 2)
        dds_set_2bytes<false>(chn, addr, 0x0);

    dds_set_4bytes<false>(chn, 0x64, magic_bytes);
}

bool Pulser::dds_exists(int chn)
{
    dds_set_2bytes<false>(chn, 0x68, 0);
    dds_get_2bytes<false>(chn, 0x68);
    dds_set_2bytes<false>(chn, 0x68, 1);
    dds_get_2bytes<false>(chn, 0x68);
    auto res0 = get_result();
    auto res1 = get_result();
    return res0 == 0 && res1 == 1;
}

void Pulser::dump_dds(std::ostream &stm, int chn)
{
    stm << "*******************************" << std::endl;

    for (unsigned addr = 0; addr + 3 <= 0x7f; addr += 4) {
        dds_get_4bytes<false>(chn, addr);
        if (uint32_t u = get_result()) {
            stm << "AD9914 board = " << chn << ", addr = 0x"
                << std::hex << addr + 3 << "..." << addr
                << " = 0x" << u << std::endl;
        }
    }
    stm << "*******************************" << std::endl;
}

bool Pulser::check_dds(int chn, bool force)
{
    if (!force) {
        // Check if magic bytes have been set (profile 7, FTW) which is
        // otherwise not used.  If already set, the board has been initialized
        // and doesn't need another init.  This avoids reboot-induced glitches.
        dds_get_4bytes<false>(chn, 0x64);
        if (get_result() == magic_bytes) {
            return false;
        }
    }
    init_dds(chn);
    return true;
}

}
