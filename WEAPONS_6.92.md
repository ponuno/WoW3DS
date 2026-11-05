# 6.92 - weapons on the character as in WoWee
- wornAttach (creature_view.cpp): sheathed main hand -> 32, 33, 26; off hand -> 33, 32, 27; shield -> 28, 12; two-hander / bow / gun -> 12, 31, 30.
- Attachment 12 (back): extra matrix T(-0.03,-0.10,0)*Rx(33)*Ry(90)*Rx(90) (w3d_back_weapon_xform, w3d_format.h), not for shields.
- Test: proto3ds/tests/worn_test.cpp (g++ -Icommon worn_test.cpp). render.cpp not compiled in the sandbox (no citro3d / shbin): build with the usual make.
