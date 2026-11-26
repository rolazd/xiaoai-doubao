#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>

namespace xiaoai_plus::app {

// Match the prototype: ASCII letters/apostrophes and U+4E00..U+9FFF runs.
inline std::vector<std::string> ConversationWords(std::string text) {
  for (size_t p=0;(p=text.find("’",p))!=std::string::npos;) text.replace(p,3,"'");
  std::vector<std::string> out;
  std::string word; int kind=0;
  for(size_t i=0;i<text.size();) {
    unsigned char c=text[i]; size_t n=1; int next=0;
    if(c<128) { if(c>='A' && c<='Z')c+=32;
      if((c>='a' && c<='z') || c=='\'')next=1;
    } else {
      n=c<0xe0?2:c<0xf0?3:4;
      if(i+n>text.size())break;
      if(n==3) { unsigned cp=((c&15)<<12)|((text[i+1]&63)<<6)|(text[i+2]&63);
        if(cp>=0x4e00 && cp<=0x9fff)next=2; }
    }
    if(next!=kind || !next) { if(!word.empty())out.push_back(word);word.clear(); }
    if(next==1)word+=char(c);else if(next==2)word+=text.substr(i,n);
    kind=next;i+=n;
  }
  if(!word.empty())out.push_back(word);
  return out;
}
inline bool LooksUnfinished(const std::string& text) {
  const auto words=ConversationWords(text);if(words.empty())return true;
  const auto& last=words.back();
  static const std::unordered_set<std::string> en={"a","about","after","am","an","and","any","are","at","be","because","been","before","being","but","by","can","could","did","do","does","er","erm","every","for","from","gonna","gotta","had","has","have","he","he's","her","his","hmm","i","i'm","if","im","in","into","is","it's","let","let's","like","love","may","might","mm","must","my","need","of","on","or","our","over","really","shall","she","she's","should","so","some","than","that's","the","their","then","there's","these","they","they're","think","this","those","to","uh","um","under","until","very","wanna","want","was","we","we're","were","when","while","will","with","would","you're","your"};
  static const std::vector<std::string> zh={"我想","我要","然后","就是","因为","所以","那个","这个","嗯","呃","额","和","跟","还有","但是","可是","如果","想问","想说","要问","要说","的是","把","给","叫","的时候","最喜欢的"};
  if(static_cast<unsigned char>(last[0])>=128) {
    for(const auto& z:zh)if(last.size()>=z.size() && last.compare(last.size()-z.size(),z.size(),z)==0)return true;
    return false;
  }
  return en.count(last)!=0;
}
inline bool ShouldCommitTurn(const std::string& text, int64_t quiet_ms,
                             int complete_ms, int open_ms) {
  return !ConversationWords(text).empty() && quiet_ms >= (LooksUnfinished(text)?open_ms:complete_ms);
}

inline std::string NormalizeConversationCommand(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n' &&
          c != '.' && c != ',' && c != '!' && c != '?' && c != ';' && c != ':')
        out.push_back(text[i]);
      ++i;
      continue;
    }
    const size_t width = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    const auto ch = text.substr(i, width);
    if (ch != "。" && ch != "，" && ch != "！" && ch != "？" &&
        ch != "、" && ch != "；" && ch != "：" && ch != "～" &&
        ch != "（" && ch != "）") out += ch;
    i += width;
  }
  return out;
}

inline size_t Utf8Characters(const std::string& text) {
  size_t count = 0;
  for (unsigned char c : text) if ((c & 0xc0) != 0x80) ++count;
  return count;
}

inline const char* MatchedExitCommand(const std::string& text) {
  static const char* commands[] = {"退出"};
  auto normalized = NormalizeConversationCommand(text);
  // Final Chinese transcript only: <=8 characters after punctuation removal,
  // whole command or a command suffix; negation/quotation/metalinguistic requests
  // never exit. No English commands. Long narratives cannot match a suffix.
  if(Utf8Characters(normalized)>8)return nullptr;
  for(const char* deny : {"不想", "不要", "不能", "别退出", "不会", "不许", "不是", "没有", "怎么", "如何", "什么", "“", "”", "\"", "翻译"}) {
    if(normalized.find(deny)!=std::string::npos)return nullptr;
  }
  for(int pass=0;pass<4;++pass) {
    for(const char* command:commands) {
      const std::string word(command);
      if(normalized==word || (normalized.size()>=word.size() &&
          normalized.compare(normalized.size()-word.size(),word.size(),word)==0))return command;
    }
    bool stripped=false;
    for(const char* particle:{"吧","啊","了","呀","啦","哦","喽"}) {
      const std::string tail(particle);
      if(normalized.size()>=tail.size() && normalized.compare(normalized.size()-tail.size(),tail.size(),tail)==0) {
        normalized.resize(normalized.size()-tail.size());stripped=true;break;
      }
    }
    if(!stripped)break;
  }
  return nullptr;
}

inline bool IsExitCommand(const std::string& text) { return MatchedExitCommand(text)!=nullptr; }

inline bool IsFarewellCommand(const std::string& text) { return IsExitCommand(text); }

}  // namespace xiaoai_plus::app
