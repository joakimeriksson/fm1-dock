// SPDX-License-Identifier: GPL-3.0-only
extern "C" void __cxa_pure_virtual() {
    for (;;) {
    }
}

extern "C" int* __errno() {
    static int e;
    return &e;
}
