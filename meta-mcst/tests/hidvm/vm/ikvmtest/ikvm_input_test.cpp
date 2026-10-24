// obmc-ikvm's Input (ikvm_input.cpp, with stand-ins for phosphor-logging and
// the Server) against the gadget, the VM being the USB host:
//   block    the host stops taking keyboard reports: writes must not block
//            (start-ipkvm's loop also drives the video)
//   wakeup   the wakeup packet (Shift) only with a VNC client connected
//   release  a client going lets go of its keys
// (-DBASE: the Input of before, without setClients/releaseAll)
#include "ikvm_server.hpp"
#include <rfb/keysym.h>
#include <rfb/rfb.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
using namespace ikvm;
int main(int argc, char** argv)
{
    std::string what = argc > 1 ? argv[1] : "";
    Input in("/dev/hidg0", "/dev/hidg1");
    Server srv;
    Server::ClientData cd{&in};
    rfbScreenInfo scr{};
    rfbClientRec cl{};
    scr.screenData = &srv;
    cl.screen = &scr;
    cl.clientData = &cd;
    if (what == "block") {
        alarm(5);
        auto t = std::chrono::steady_clock::now();
        for (int i = 0; i < 4; i++) {
            Input::keyEvent(TRUE, XK_a, &cl); in.sendReport();
            Input::keyEvent(FALSE, XK_a, &cl); in.sendReport();
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        printf("8 reports in %.0f ms\n", ms);
        return 0;
    }
    if (what == "rebind") {
        // a key, the gadget unbound and bound again (the test makes /tmp/ikvm-go), a key
        Input::keyEvent(TRUE, XK_a, &cl); in.sendReport();
        Input::keyEvent(FALSE, XK_a, &cl); in.sendReport();
        unlink("/tmp/ikvm-go");
        for (int i = 0; i < 200 && access("/tmp/ikvm-go", F_OK); i++) usleep(50000);
        Input::keyEvent(TRUE, XK_b, &cl); in.sendReport(); // may fail: the fd of before
        usleep(100000);
        Input::keyEvent(FALSE, XK_b, &cl); in.sendReport();
        usleep(100000);
        Input::keyEvent(TRUE, XK_b, &cl); in.sendReport();
        usleep(100000);
        Input::keyEvent(FALSE, XK_b, &cl); in.sendReport();
        usleep(300000);
        return 0;
    }
    if (what == "wakeup0") { in.sendWakeupPacket(); return 0; }
    if (what == "wakeup1") {
#ifndef BASE
        in.setClients(1);
#endif
        in.sendWakeupPacket();
        return 0;
    }
    if (what == "release") {
#ifndef BASE
        in.setClients(1);
#endif
        Input::keyEvent(TRUE, XK_a, &cl); in.sendReport();
        usleep(300000);
#ifndef BASE
        in.releaseAll(); // what Server::clientGone does now
        in.setClients(0);
#endif
        usleep(300000);
        return 0;
    }
    return 2;
}
