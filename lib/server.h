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

#ifndef LIBMOLECUBE_SERVER_H
#define LIBMOLECUBE_SERVER_H

#include "ctrl_iface.h"

#include <nacs-utils/zmq_utils.h>

namespace Molecube {

using namespace NaCs;

class Config;

class NACS_PROTECTED() Server {
public:
    Server(const Config &conf);

private:
    const Config &m_conf;
    const uint64_t m_id;
    std::unique_ptr<CtrlIFace> m_ctrl;
};

}

#endif // LIBMOLECUBE_SERVER_H
