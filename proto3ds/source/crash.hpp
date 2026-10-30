// crash.hpp — stage 6.31: crash.txt on a CPU exception (crash.cpp).
#pragma once
void crashInstall();       // in every thread, at its start (libctru threadOnException is per thread)
void crashCheckCmdBuf();   // once a frame, right after C3D_FrameBegin
