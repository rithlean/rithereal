// SPDX-License-Identifier: Apache-2.0
#include "rith/liberty.hpp"

#include <cctype>
#include <fstream>
#include <istream>
#include <iterator>
#include <stdexcept>

#include "rith/io.hpp"

namespace rith {

namespace {

// ---------------------------------------------------------------------------
// Generic Liberty syntax: groups, simple attributes and complex attributes.
// ---------------------------------------------------------------------------
struct LTok {
  enum Kind { Word, String, Sym, End } kind = End;
  std::string text;
  int line = 0;
};

std::vector<LTok> lex(const std::string& s, const std::string& file) {
  std::vector<LTok> out;
  std::size_t i = 0;
  int line = 1;
  auto is_word_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-' ||
           c == '+' || c == '[' || c == ']' || c == '!' || c == '\'' || c == '*' || c == '&' ||
           c == '|' || c == '^' || c == '/' || c == '$' || c == '<' || c == '>' || c == '=' || c == '@';
  };
  while (i < s.size()) {
    const char c = s[i];
    if (c == '\n') { ++line; ++i; continue; }
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    if (c == '\\' && i + 1 < s.size() && (s[i + 1] == '\n' || s[i + 1] == '\r')) { ++i; continue; }
    if (s.compare(i, 2, "/*") == 0) {
      const auto e = s.find("*/", i + 2);
      if (e == std::string::npos) throw ParseError(file, line, "unterminated comment");
      for (std::size_t k = i; k < e; ++k) line += s[k] == '\n';
      i = e + 2;
      continue;
    }
    if (s.compare(i, 2, "//") == 0) { while (i < s.size() && s[i] != '\n') ++i; continue; }
    LTok t;
    t.line = line;
    if (c == '"') {
      std::size_t j = i + 1;
      std::string v;
      while (j < s.size() && s[j] != '"') {
        if (s[j] == '\\' && j + 1 < s.size() && (s[j + 1] == '\n' || s[j + 1] == '\r')) { ++j; continue; }
        if (s[j] == '\n') ++line;
        v += s[j++];
      }
      if (j >= s.size()) throw ParseError(file, t.line, "unterminated string");
      t.kind = LTok::String;
      t.text = std::move(v);
      i = j + 1;
    } else if (c == '{' || c == '}' || c == '(' || c == ')' || c == ':' || c == ';' || c == ',') {
      t.kind = LTok::Sym;
      t.text = std::string(1, c);
      ++i;
    } else if (is_word_char(c)) {
      std::size_t j = i;
      while (j < s.size() && is_word_char(s[j])) ++j;
      t.kind = LTok::Word;
      t.text = s.substr(i, j - i);
      i = j;
    } else {
      throw ParseError(file, line, std::string("unexpected character '") + c + "'");
    }
    out.push_back(std::move(t));
  }
  LTok end;
  end.line = line;
  out.push_back(end);
  return out;
}

struct Group {
  std::string type;
  std::vector<std::string> args;
  std::vector<std::pair<std::string, std::string>> attrs;  // simple: name -> value
  std::vector<Group> groups;
  int line = 0;

  std::string attr(const std::string& n) const {
    for (const auto& [k, v] : attrs)
      if (k == n) return v;
    return {};
  }
};

class GroupParser {
 public:
  GroupParser(std::vector<LTok> t, std::string file) : t_(std::move(t)), file_(std::move(file)) {}

  Group top() {
    Group g = statement_group();
    return g;
  }

 private:
  const LTok& peek() const { return t_[p_]; }
  const LTok& next() { const LTok& t = t_[p_]; if (p_ + 1 < t_.size()) ++p_; return t; }
  bool sym(const char* s) const { return peek().kind == LTok::Sym && peek().text == s; }
  [[noreturn]] void fail(const std::string& m) const { throw ParseError(file_, peek().line, m); }
  void expect(const char* s) {
    if (!sym(s)) fail(std::string("expected '") + s + "' but found '" + peek().text + "'");
    next();
  }

  // name ( args ) { ... }  -- the opening name already peeked
  Group statement_group() {
    if (peek().kind != LTok::Word) fail("expected a group name");
    Group g;
    g.line = peek().line;
    g.type = next().text;
    expect("(");
    g.args = arg_list();
    expect("{");
    body(g);
    return g;
  }

  std::vector<std::string> arg_list() {
    std::vector<std::string> args;
    while (!sym(")")) {
      if (peek().kind == LTok::End) fail("unexpected end of file in an argument list");
      if (sym(",")) { next(); continue; }
      args.push_back(next().text);
    }
    next();
    return args;
  }

  void body(Group& g) {
    while (!sym("}")) {
      if (peek().kind == LTok::End) fail("missing '}' for group '" + g.type + "'");
      if (peek().kind != LTok::Word) fail("unexpected '" + peek().text + "'");
      const std::string name = next().text;
      if (sym(":")) {  // simple attribute
        next();
        std::string value;
        while (!sym(";") && !sym("}")) {
          if (peek().kind == LTok::End) fail("unexpected end of file in attribute '" + name + "'");
          if (!value.empty()) value += ' ';
          value += next().text;
        }
        if (sym(";")) next();
        g.attrs.emplace_back(name, value);
      } else if (sym("(")) {
        next();
        auto args = arg_list();
        if (sym("{")) {
          next();
          Group child;
          child.type = name;
          child.args = std::move(args);
          child.line = peek().line;
          body(child);
          g.groups.push_back(std::move(child));
        } else {  // complex attribute: keep the first argument
          if (sym(";")) next();
          g.attrs.emplace_back(name, args.empty() ? std::string() : args.front());
        }
      } else {
        fail("expected ':' or '(' after '" + name + "'");
      }
    }
    next();
  }

  std::vector<LTok> t_;
  std::string file_;
  std::size_t p_ = 0;
};

LibCell make_cell(const Group& g) {
  LibCell cell;
  cell.name = g.args.empty() ? std::string() : g.args.front();
  for (const auto& sub : g.groups) {
    if (sub.type == "pin") {
      for (const auto& pn : sub.args) {
        LibPin p;
        p.name = pn;
        const std::string d = sub.attr("direction");
        p.dir = d == "output" ? LibPin::Dir::Output
              : d == "inout" ? LibPin::Dir::Inout
              : d == "internal" ? LibPin::Dir::Internal
              : LibPin::Dir::Input;
        p.function = sub.attr("function");
        p.signal_type = sub.attr("signal_type");
        if (p.signal_type.rfind("test_scan", 0) == 0) cell.is_scan = true;
        cell.pins.push_back(std::move(p));
      }
    } else if (sub.type == "ff") {
      LibFlop f;
      if (sub.args.size() >= 1) f.iq = sub.args[0];
      if (sub.args.size() >= 2) f.iqn = sub.args[1];
      f.next_state = sub.attr("next_state");
      f.clocked_on = sub.attr("clocked_on");
      f.clear = sub.attr("clear");
      f.preset = sub.attr("preset");
      cell.flop = f;
    } else if (sub.type == "latch") {
      LibLatch l;
      if (sub.args.size() >= 1) l.iq = sub.args[0];
      if (sub.args.size() >= 2) l.iqn = sub.args[1];
      l.data_in = sub.attr("data_in");
      l.enable = sub.attr("enable");
      cell.latch = l;
      cell.has_latch = true;
    } else if (sub.type == "statetable") {
      cell.has_statetable = true;
    } else if (sub.type == "test_cell") {
      cell.is_scan = true;
      for (const auto& tp : sub.groups)  // signal types live in the test_cell's pins
        if (tp.type == "pin")
          for (const auto& pn : tp.args)
            for (auto& p : cell.pins)
              if (p.name == pn && p.signal_type.empty()) p.signal_type = tp.attr("signal_type");
    }
  }
  return cell;
}

// ---------------------------------------------------------------------------
// Boolean function parser
// ---------------------------------------------------------------------------
class ExprParser {
 public:
  explicit ExprParser(const std::string& s) : s_(s) {}

  LibExpr parse() {
    LibExpr e = parse_or();
    skip();
    if (i_ != s_.size()) fail("unexpected '" + s_.substr(i_) + "'");
    return e;
  }

 private:
  [[noreturn]] void fail(const std::string& m) const {
    throw std::runtime_error("bad Liberty function \"" + s_ + "\": " + m);
  }
  void skip() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
  bool at(char c) { skip(); return i_ < s_.size() && s_[i_] == c; }
  bool starts_operand() {
    skip();
    if (i_ >= s_.size()) return false;
    const char c = s_[i_];
    return c == '(' || c == '!' || std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\\';
  }
  static LibExpr binary(LibExpr::Op op, LibExpr a, LibExpr b) {
    LibExpr e;
    e.op = op;
    if (a.op == op) e.args = std::move(a.args); else e.args.push_back(std::move(a));
    if (b.op == op) for (auto& x : b.args) e.args.push_back(std::move(x));
    else e.args.push_back(std::move(b));
    return e;
  }

  LibExpr parse_or() {
    LibExpr e = parse_and();
    while (at('|') || at('+')) { ++i_; e = binary(LibExpr::Op::Or, std::move(e), parse_and()); }
    return e;
  }
  LibExpr parse_and() {
    LibExpr e = parse_xor();
    while (true) {
      if (at('&') || at('*')) { ++i_; e = binary(LibExpr::Op::And, std::move(e), parse_xor()); }
      else if (starts_operand()) e = binary(LibExpr::Op::And, std::move(e), parse_xor());  // juxtaposition
      else break;
    }
    return e;
  }
  LibExpr parse_xor() {
    LibExpr e = parse_unary();
    while (at('^')) { ++i_; e = binary(LibExpr::Op::Xor, std::move(e), parse_unary()); }
    return e;
  }
  LibExpr parse_unary() {
    if (at('!')) {
      ++i_;
      LibExpr e;
      e.op = LibExpr::Op::Not;
      e.args.push_back(parse_unary());
      return e;
    }
    LibExpr e = parse_primary();
    while (at('\'')) {
      ++i_;
      LibExpr n;
      n.op = LibExpr::Op::Not;
      n.args.push_back(std::move(e));
      e = std::move(n);
    }
    return e;
  }
  LibExpr parse_primary() {
    skip();
    if (i_ >= s_.size()) fail("unexpected end");
    if (s_[i_] == '(') {
      ++i_;
      LibExpr e = parse_or();
      if (!at(')')) fail("missing ')'");
      ++i_;
      return e;
    }
    std::size_t j = i_;
    while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_' || s_[j] == '[' ||
                             s_[j] == ']' || s_[j] == '.'))
      ++j;
    if (j == i_) fail(std::string("unexpected '") + s_[i_] + "'");
    LibExpr e;
    e.var = s_.substr(i_, j - i_);
    i_ = j;
    if (e.var == "0") e.op = LibExpr::Op::Const0;
    else if (e.var == "1") e.op = LibExpr::Op::Const1;
    return e;
  }

  const std::string& s_;
  std::size_t i_ = 0;
};

}  // namespace

LibExpr parse_liberty_function(const std::string& text) { return ExprParser(text).parse(); }

Library read_liberty(std::istream& in, const std::string& filename) {
  std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const Group top = GroupParser(lex(src, filename), filename).top();
  if (top.type != "library") throw ParseError(filename, top.line, "expected a library group");
  Library lib;
  lib.name = top.args.empty() ? std::string() : top.args.front();
  for (const auto& g : top.groups)
    if (g.type == "cell") {
      LibCell c = make_cell(g);
      lib.cells[c.name] = std::move(c);
    }
  return lib;
}

Library read_liberty_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open '" + path + "'");
  return read_liberty(in, path);
}

void merge_into(Library& dst, Library&& src) {
  if (dst.name.empty()) dst.name = src.name;
  for (auto& [n, c] : src.cells) dst.cells[n] = std::move(c);
}

}  // namespace rith
