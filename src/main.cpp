#include <iostream>

#include "appstate.hpp"

AppState state;

int main(int argc, char** argv) {
    if (!state.open("/mnt/d/Media/wan/cf20260815_214406.mp4", "/home/erics/test.mp4"))
        return -1;

    state.process();
    
    return 0;
}
