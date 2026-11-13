#include "engine/core/Input.h"
#include <cstdio>
#include <stdexcept>
void Require(bool ok){if(!ok)throw std::runtime_error("input event regression");}
int main(){
    try {
        Input in;
        in.OnKeyDown('R');in.OnKeyUp('R');in.BeginFrame(nullptr);
        Require(in.IsKeyDown('R'));in.BeginFrame(nullptr);Require(!in.IsKeyDown('R'));
        in.OnKeyDown('W');in.BeginFrame(nullptr);Require(in.IsKeyDown('W'));
        in.BeginFrame(nullptr);Require(in.IsKeyDown('W'));
        in.OnKeyUp('W');in.BeginFrame(nullptr);Require(!in.IsKeyDown('W'));
        in.OnKeyDown(VK_LBUTTON);in.OnKeyUp(VK_LBUTTON);in.BeginFrame(nullptr);Require(in.IsKeyDown(VK_LBUTTON));
        in.OnFocusLost();Require(!in.IsKeyDown(VK_LBUTTON));
        in.OnKeyDown('R');in.OnFocusLost();in.BeginFrame(nullptr);Require(!in.IsKeyDown('R'));
        Require(!in.IsKeyDown(-1)&&!in.IsKeyDown(256));
        puts("PASS quick taps, clicks, held keys, release and focus loss");return 0;
    }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
