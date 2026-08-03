#include "frontend/lexer.hpp"

#include <iostream>

using namespace luatier;

static int expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "lexer FAIL: " << msg << "\n";
    return 1;
  }
  return 0;
}

int test_lexer() {
  int f = 0;
  Lexer lex("local x = 1 + 2 -- comment\nreturn x");
  f += expect(lex.next().kind == TokenKind::KwLocal, "local");
  f += expect(lex.next().kind == TokenKind::Identifier, "x");
  f += expect(lex.next().kind == TokenKind::Eq, "=");
  f += expect(lex.next().kind == TokenKind::Integer, "1");
  f += expect(lex.next().kind == TokenKind::Plus, "+");
  f += expect(lex.next().kind == TokenKind::Integer, "2");
  f += expect(lex.next().kind == TokenKind::KwReturn, "return");
  f += expect(lex.next().kind == TokenKind::Identifier, "x2");
  f += expect(lex.next().kind == TokenKind::End, "eof");
  return f ? 1 : 0;
}
