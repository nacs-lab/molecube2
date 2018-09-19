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

#include "namesconfig.h"

#include <nacs-utils/log.h>

#include <yaml-cpp/yaml.h>

#include <fstream>

namespace Molecube {

NACS_PROTECTED() void NamesConfig::save()
{
    std::ofstream fstm(m_fname);
    if (!fstm.good()) {
        Log::error("Cannot open %s for saving.", m_fname.c_str());
        return;
    }
    YAML::Emitter yaml;
    yaml << m_names;
    fstm << yaml.c_str();
}

NACS_PROTECTED() void NamesConfig::load()
{
    YAML::Node file;
    try {
        file = YAML::LoadFile(m_fname);
    }
    catch (const YAML::Exception &err) {
        Log::error("%s\n", err.what());
        return;
    }
    if (!file.IsSequence()) {
        Log::error("File %s not a list\n", m_fname.c_str());
        return;
    }
    m_names.clear();
    for (const auto &p: file) {
        m_names.push_back(p.as<std::string>());
    }
}

}
