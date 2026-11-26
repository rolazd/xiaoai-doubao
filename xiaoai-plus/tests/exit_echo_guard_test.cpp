#ifdef NDEBUG
#error requires active asserts
#endif
#include <cassert>
#include <iostream>
#include "wakeup/exit_echo_guard.hpp"
int main(int argc,char** argv) {
  using namespace xiaoai_plus::wakeup;
  ExitEchoGuard guard;
  if(argc==3) {guard.Update("replay",argv[1],true);std::cout<<guard.Contains(argv[2]);return 0;}
  if(argc==2) {std::cout<<StrongExitLabel(argv[1]);return 0;}
  assert(StrongExitWord(StrongExitLabel("t uì ch ū #0.30 @结束"))=="退出");
  assert(StrongExitLabel("b ì z uǐ #0.30 @结束").empty());
  for(const char* word:{"退出"}) {
    const std::string w(word),label="强退:"+w;
    guard.Reset();guard.Update("r1","可以说"+w.substr(0,3),false);
    assert(!guard.Contains(label));guard.Update("r1",w.substr(3),false);
    assert(guard.Contains(label));guard.Update("r1",std::string(100000,'x'),false);
    assert(guard.Contains(label));guard.Update("r1","我们聊恐龙",true);
    assert(!guard.Contains(label));guard.Update("r2",w,true);assert(guard.Contains(label));
    guard.Update("r3","新回答",false);assert(!guard.Contains(label));
    guard.Update("r3",w,false);guard.Update("","",false,true);assert(!guard.Contains(label));
    guard.Update("","可 以 说 "+w,false);assert(guard.Contains(label));
    guard.Reset();assert(!guard.Contains(label));
  }
  guard.Update("r","关掉、停止",true);
  assert(!guard.Contains("强退:关闭"));
  assert(!guard.Contains("强退:停下"));
}
