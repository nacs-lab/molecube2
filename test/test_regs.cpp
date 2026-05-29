#include "../lib/pulser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string_view>

int main(int argc, char **argv)
{
    auto addr = Molecube::Pulser::address();
    if (!addr) {
        printf("Cannot find pulser\n");
        return 1;
    }
    Molecube::Pulser p(addr);

    int argi = 0;
    auto next_arg = [&] (bool check=true) {
        auto arg = argv[++argi];
        if (check && !arg)
            throw std::invalid_argument("Missing argument");
        return arg;
    };

    while (auto _arg = next_arg(false)) {
        std::string_view arg(_arg);
        if (arg == "r") {
            int idx = atoi(next_arg());
            printf("Read [%d]: %08x\n", idx, p.read(idx));
        }
        else if (arg == "w") {
            int idx = atoi(next_arg());
            int val = atoi(next_arg());
            printf("Write [%d]: %08x\n", idx, val);
            p.write(idx, val);
        }
        else {
            throw std::invalid_argument("Unknown argument");
        }
    }

    return 0;
}
