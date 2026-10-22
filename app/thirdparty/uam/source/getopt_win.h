// Native Windows command-line adapter for uam's small, fixed option set.
// SPDX-License-Identifier: Zlib
#pragma once
#include <cstring>
enum { no_argument = 0, required_argument = 1 };
struct option { const char *name; int has_arg; int *flag; int val; };
static char *optarg = nullptr;
static int optind = 1;
static int getopt_long(int argc, char **argv, const char *shorts,
                       const option *options, int *index) {
    if (optind >= argc || argv[optind][0] != '-' || !argv[optind][1]) return -1;
    char *arg = argv[optind++];
    if (!std::strcmp(arg, "--")) return -1;
    optarg = nullptr;
    int value = '?', needs = no_argument;
    if (arg[1] == '-') {
        const char *name = arg + 2;
        const char *equal = std::strchr(name, '=');
        const size_t length = equal ? size_t(equal - name) : std::strlen(name);
        for (int i = 0; options[i].name; ++i) {
            if (std::strlen(options[i].name) != length || std::strncmp(name, options[i].name, length)) continue;
            if (index) *index = i;
            value = options[i].val;
            needs = options[i].has_arg;
            if (equal) optarg = const_cast<char *>(equal + 1);
            break;
        }
    } else {
        const char *found = std::strchr(shorts, arg[1]);
        if (found) { value = arg[1]; needs = found[1] == ':'; }
        if (arg[2]) optarg = arg + 2;
    }
    if (needs == required_argument && !optarg) {
        if (optind >= argc) return '?';
        optarg = argv[optind++];
    }
    return value;
}
