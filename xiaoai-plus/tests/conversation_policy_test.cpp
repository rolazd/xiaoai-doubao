#include <cassert>

#include "app/conversation_policy.hpp"

int main() {
  using namespace xiaoai_plus::app;
  assert(LooksUnfinished("I want to")==true);
  assert(LooksUnfinished("My favorite animal is")==true);
  assert(LooksUnfinished("I like")==true);
  assert(LooksUnfinished("我想问")==true);
  assert(LooksUnfinished("I want")==true);
  assert(LooksUnfinished("and then")==true);
  assert(LooksUnfinished("I'm")==true);
  assert(LooksUnfinished("Because")==true);
  assert(LooksUnfinished("um")==true);
  assert(LooksUnfinished("It's a")==true);
  assert(LooksUnfinished("My name is")==true);
  assert(LooksUnfinished("I think")==true);
  assert(LooksUnfinished("我最喜欢的是")==true);
  assert(LooksUnfinished("然后")==true);
  assert(LooksUnfinished("I like dogs.")==false);
  assert(LooksUnfinished("Story!")==false);
  assert(LooksUnfinished("Riddle")==false);
  assert(LooksUnfinished("Yes")==false);
  assert(LooksUnfinished("No")==false);
  assert(LooksUnfinished("Amy")==false);
  assert(LooksUnfinished("I am Amy.")==false);
  assert(LooksUnfinished("Thank you")==false);
  assert(LooksUnfinished("What is it?")==false);
  assert(LooksUnfinished("A banana!")==false);
  assert(LooksUnfinished("Me too")==false);
  assert(LooksUnfinished("One more")==false);
  assert(LooksUnfinished("Let's go")==false);
  assert(LooksUnfinished("I like it")==false);
  assert(LooksUnfinished("apple 用中文怎么说？")==false);
  assert(LooksUnfinished("Guess the animal")==false);
  assert(LooksUnfinished("Is it a cat?")==false);
  assert(LooksUnfinished("Simon says jump")==false);
  assert(LooksUnfinished("我想听故事")==false);
  assert(LooksUnfinished("好的")==false);
  assert(LooksUnfinished("是")==false);
  assert(LooksUnfinished("我在")==false);
  assert(LooksUnfinished("怎么说")==false);
  assert(LooksUnfinished("我想说")==true);
  assert(LooksUnfinished("它叫")==true);
  assert(LooksUnfinished("我喜欢的是")==true);
  assert(LooksUnfinished("Who's there?")==false);
  assert(!ShouldCommitTurn("I like",4499,1500,4500));
  assert(ShouldCommitTurn("I like",4500,1500,4500));
  assert(!ShouldCommitTurn("Yes",1499,1500,4500));
  assert(ShouldCommitTurn("Yes",1500,1500,4500));
  assert(!ShouldCommitTurn("...",999999,1500,4500));
  assert(ConversationWords("I like dogs.")==ConversationWords("i LIKE dogs!"));
  assert(ConversationWords("I’m")==ConversationWords("I'm"));

  using xiaoai_plus::app::IsExitCommand;
  using xiaoai_plus::app::IsFarewellCommand;
  assert(IsExitCommand("退出"));
  assert(IsExitCommand(" 退出。 "));
  assert(IsExitCommand("退出吧！"));
  for (const char* word : {"再见", "拜拜", "豆包再见", "关闭", "关掉",
                           "停止", "停掉", "停下", "退下", "结束",
                           "结束对话", "不聊了", "别说了", "闭嘴", "退出对话"}) {
    assert(!IsExitCommand(word));
  }
  assert(IsExitCommand("请你先退出呀"));
  for(const char* text:{"好了","我猜好了","好 了","停"})assert(!IsExitCommand(text));
  assert(std::string(xiaoai_plus::app::MatchedExitCommand("退出"))=="退出");
  assert(xiaoai_plus::app::MatchedExitCommand("停下")==nullptr);
  assert(xiaoai_plus::app::MatchedExitCommand("再见")==nullptr);
  assert(!IsExitCommand("退出按钮在哪里"));
  assert(!IsExitCommand("我不想退出"));
  for(const char* t:{"我们明天再见到恐龙","不要关闭","恐龙说再见","请翻译再见","bye","goodbye","stop","我说的是“再见”"})assert(!IsExitCommand(t));
  assert(!IsExitCommand("今天先到这里，再见"));
  assert(IsFarewellCommand("退出。"));
  assert(!IsFarewellCommand("再见。"));
  assert(!IsFarewellCommand("豆包再见"));
  assert(!IsFarewellCommand("拜拜！"));
}
