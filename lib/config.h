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

#ifndef LIBMOLECUBE_CONFIG_H
#define LIBMOLECUBE_CONFIG_H

#include "ctrl_iface.h"

#include <string>

namespace Molecube {

using namespace NaCs;

struct Config {
    Config();
    static Config loadYAML(const char *fname);

    bool dummy = false;
    std::string listen{"tcp://*:7777"};
    std::string runtime_dir{"/var/lib/molecube/"};
};

}

#endif // LIBMOLECUBE_CONFIG_H
