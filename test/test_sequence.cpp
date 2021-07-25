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

#include "../lib/ctrl_iface.h"

#include <nacs-utils/errors.h>
#include <nacs-utils/streams.h>
#include <nacs-seq/zynq/cmdlist.h>

#include <fstream>
#include <iostream>

#include <inttypes.h>

using namespace NaCs;
using namespace Molecube;

struct sequence {
    std::string seq;
    uint32_t ttl_mask;
    uint64_t len_ns;
};

static sequence parse_file(const char *name)
{
    std::ifstream istm(name);
    string_ostream sstm;
    sequence res;
    res.ttl_mask = Seq::Zynq::CmdList::parse(sstm, istm, 1);
    res.seq = sstm.get_buf();
    res.len_ns = Seq::Zynq::CmdList::total_time((uint8_t*)res.seq.data(),
                                                res.seq.size(), 1) * 10;
    printf("Sequence length: %" PRIu64 " ns\n", res.len_ns);
    return res;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("Require one argument: test_sequence seq_file\n");
        return 1;
    }

    sequence seq;
    try {
        seq = parse_file(argv[1]);
    }
    catch (const SyntaxError &err) {
        std::cerr << "Error parsing startup script:\n" << err;
        return 1;
    }

    auto ctrl = Molecube::CtrlIFace::create();

    bool finished = false;
    struct Notify: CtrlIFace::ReqSeqNotify {
        Notify(bool *finished)
            : finished(finished)
        {
        }
        ~Notify() override
        {
            *finished = true;
        }
        bool *finished;
    };
    ctrl->run_code(true, 1, seq.len_ns, seq.ttl_mask, (const uint8_t*)seq.seq.data(),
                   seq.seq.size(), std::make_unique<Notify>(&finished));
    while (!finished) {
        using namespace std::literals;
        std::this_thread::sleep_for(1ms);
        ctrl->run_frontend();
    }
    printf("Sequence finished.\n");

    return 0;
}
