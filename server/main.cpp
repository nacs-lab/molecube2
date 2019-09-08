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

#include "../lib/server.h"
#include "../lib/config.h"

#include <nacs-utils/log.h>

#include <signal.h>

#include <thread>

using namespace Molecube;

// Block the sigint before starting any threads so that it's blocked for the whole process.
// This avoid having to deal with failed system call (especially poll).
static void block_sigint()
{
    sigset_t sset;
    sigemptyset(&sset);
    sigaddset(&sset, SIGINT);
    if (auto s = pthread_sigmask(SIG_BLOCK, &sset, NULL)) {
        errno = s;
        perror("Failed to setup signal mask.");
        return;
    }
}

static std::atomic_bool stop{false};
static std::thread sigthread;

static void setup_signal(Server *server)
{
    sigthread = std::thread([server] () {
        sigset_t sset;
        sigemptyset(&sset);
        sigaddset(&sset, SIGINT);
        timespec timeout;
        timeout.tv_sec = 0;
        timeout.tv_nsec = 50000000; // 50ms

        while (!stop.load(std::memory_order_relaxed)) {
            // The timeout is needed so that we can notice when `stop` is set.
            auto s = sigtimedwait(&sset, nullptr, &timeout);
            if (s < 0) {
                // Timeout
                if (errno == EAGAIN)
                    continue;
                perror("sigtimedwait errors.");
                exit(1);
            }
            assert(s == SIGINT);
            Log::info("Stopping server.\n");
            server->stop();
        }
    });
}

int main(int argc, char **argv)
{
    // TODO: better arguments handling
    auto config = Config::loadYAML(argc >= 2 ? argv[1] : "/etc/molecube.yml");

    block_sigint();
    Server server(config);
    setup_signal(&server);
    server.run();
    stop.store(true, std::memory_order_relaxed);
    sigthread.join();

    return 0;
}
