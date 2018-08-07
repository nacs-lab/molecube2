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

#include "controller.h"

#include <nacs-kernel/devctl.h>

#include <chrono>

namespace Molecube {

Controller::Controller()
    : m_addr(Kernel::mapPulseCtrl())
{
}

bool Controller::concurrent_set(ReqOP op, uint32_t operand, bool is_override,
                                uint32_t val)
{
    if (op != TTL)
        return false;
    if (!is_override)
        return false;
    if (operand == 0) {
        set_ttl_lomask(val);
        return true;
    }
    else if (operand == 1) {
        set_ttl_himask(val);
        return true;
    }
    return false;
}

bool Controller::concurrent_get(ReqOP op, uint32_t operand, bool is_override,
                                uint32_t &val)
{
    if (op == Clock) {
        val = cur_clock();
        return false;
    }
    if (op != TTL)
        return false;
    if (!is_override) {
        if (operand != 0)
            return false;
        val = (cur_ttl() | ttl_himask()) & ~ttl_lomask();
        return true;
    }
    if (operand == 0) {
        val = ttl_lomask();
        return true;
    }
    else if (operand == 1) {
        val = ttl_himask();
        return true;
    }
    return false;
}

bool Controller::try_get_result(uint32_t &res)
{
    if (!num_results())
        return false;
    res = pop_result();
    return true;
}

uint32_t Controller::get_result()
{
    // Used in cases where we don't care about the performance
    // of the calling thread too much.
    uint32_t res;
    while (!try_get_result(res))
        std::this_thread::yield();
    return res;
}

void Controller::init_dds(int chn)
{
    using namespace std::literals;

    dds_reset_pulse<false>(chn);

    // calibrate internal timing.  required at power-up
    dds_set_2bytes_pulse<false>(chn, 0x0e, 0x0105);
    std::this_thread::sleep_for(1ms);
    // finish cal. disble sync_out
    dds_set_2bytes_pulse<false>(chn, 0x0e, 0x0405);

    // enable programmable modulus and profile 0, enable SYNC_CLK output
    // dds_set_2bytes_pulse<false>(chn, 0x05, 0x8d0b);

    // disable programmable modulus, enable profile 0,
    // enable SYNC_CLK output
    // dds_set_2bytes_pulse<false>(chn, 0x05, 0x8009);

    // disable ramp & programmable modulus, enable profile 0,
    // disable SYNC_CLK output
    // dds_set_2bytes_pulse<false>(chn, 0x05, 0x8001);

    // disable SYNC_CLK output
    dds_set_2bytes_pulse<false>(chn, 0x04, 0x0100);

    // enable ramp, enable programmable modulus, disable profile mode
    // dds_set_2bytes_pulse<false>(chn, 0x06, 0x0009);

    // disable ramp, disable programmable modulus, enable profile mode
    dds_set_2bytes_pulse<false>(chn, 0x06, 0x0080);

    // enable amplitude control (OSK)
    dds_set_2bytes_pulse<false>(chn, 0x0, 0x0308);

    // zero-out all other memory
    for (unsigned addr = 0x10;addr <= 0x6a;addr += 2)
        dds_set_2bytes_pulse<false>(chn, addr, 0x0);

    dds_set_4bytes_pulse<false>(chn, 0x64, magic_bytes);
}

bool Controller::check_dds(int chn)
{
    if (!m_dds_pending_reset[chn]) {
        // Check if magic bytes have been set (profile 7, FTW) which is
        // otherwise not used.  If already set, the board has been initialized
        // and doesn't need another init.  This avoids reboot-induced glitches.
        dds_get_4bytes_pulse<false>(chn, 0x64);
        if (get_result() == magic_bytes) {
            return false;
        }
    }
    init_dds(chn);
    m_dds_pending_reset[chn] = false;
    return true;
}

}
