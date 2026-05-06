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

#include <nacs-kernel/devctl.h>

#include <fstream>
#include <iostream>

#include <inttypes.h>

using namespace NaCs;
using namespace Molecube;

int main(int argc, char **argv)
{
    auto addr = Molecube::Pulser::address();
    printf("%p\n", addr);
    auto buff1 = Kernel::allocDMABuffer(16 * 4096);
    auto buff2 = Kernel::allocDMABuffer(16 * 4096);
    auto buff3 = Kernel::allocOCMBuffer(16 * 4096);
    auto buff4 = Kernel::allocOCMBuffer(16 * 4096);
    printf("%p, %p, %p, %p\n", buff1, buff2, buff3, buff4);
    if (buff1)
        Kernel::freeDMABuffer(buff1, 16 * 4096);
    if (buff2)
        Kernel::freeDMABuffer(buff2, 16 * 4096);
    if (buff3)
        Kernel::freeOCMBuffer(buff3, 16 * 4096);
    if (buff4)
        Kernel::freeOCMBuffer(buff4, 16 * 4096);
    return 0;
}
