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

#include "config.h"

#include <yaml-cpp/yaml.h>

namespace Molecube {

NACS_PROTECTED() Config::Config()
{
}

NACS_PROTECTED() Config Config::loadYAML(const char *fname)
{
    Config conf;
    auto file = YAML::LoadFile(fname);

    if (auto dummy_node = file["dummy"])
        conf.dummy = dummy_node.as<bool>();
    if (auto listen_node = file["listen"])
        conf.listen = listen_node.as<std::string>();
    if (auto runtime_dir_node = file["runtime_dir"])
        conf.runtime_dir = runtime_dir_node.as<std::string>();

    return conf;
}

}
