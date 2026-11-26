// SPDX-License-Identifier: Apache-2.0
#include <cctype>
#include <istream>
#include <iterator>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "rith/io.hpp"

namespace rith {

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------
namespace {

struct Tok {
  enum Kind { Ident, Number, Sym, End } kind = End;
  std::string text;
  int line = 0;
};

class Lexer {
 public:
  Lexer(std::string src, std::string file) : s_(std::move(src)), file_(std::move(file)) {}

  std::vector<Tok> run() {
    std::vector<Tok> out;
    while (true) {
      skip();
      if (i_ >= s_.size()) break;
      const char c = s_[i_];
      Tok t;
      t.line = line_;
      if (c == '\\') {  // escaped identifier: up to the next whitespace
        std::size_t j = ++i_;
        while (j < s_.size() && !std::isspace(static_cast<unsigned char>(s_[j]))) ++j;
        t.kind = Tok::Ident;
        t.text = s_.substr(i_, j - i_);
        i_ = j;
      } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
        std::size_t j = i_ + 1;
        while (j < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[j])) || s_[j] == '_' || s_[j] == '$')) ++j;
        t.kind = Tok::Ident;
        t.text = s_.substr(i_, j - i_);
        i_ = j;
      } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '\'') {
        std::size_t j = i_;
        while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) ++j;
        if (j < s_.size() && s_[j] == '\'') {
          ++j;
          if (j < s_.size() && (s_[j] == 's' || s_[j] == 'S')) ++j;
          if (j < s_.size()) ++j;  // base character
          while (j < s_.size() && (std::isxdigit(static_cast<unsigned char>(s_[j])) || s_[j] == '_' ||
                                   s_[j] == 'x' || s_[j] == 'X' || s_[j] == 'z' || s_[j] == 'Z' || s_[j] == '?'))
            ++j;
        }
        t.kind = Tok::Number;
        t.text = s_.substr(i_, j - i_);
        i_ = j;
      } else {
        t.kind = Tok::Sym;
        t.text = std::string(1, c);
        ++i_;
      }
      out.push_back(std::move(t));
    }
    Tok end;
    end.line = line_;
    out.push_back(end);
    return out;
  }

 private:
  void skip() {
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == '\n') { ++line_; ++i_; }
      else if (std::isspace(static_cast<unsigned char>(c))) ++i_;
      else if (s_.compare(i_, 2, "//") == 0) { while (i_ < s_.size() && s_[i_] != '\n') ++i_; }
      else if (s_.compare(i_, 2, "/*") == 0) { block("*/"); }
      else if (s_.compare(i_, 2, "(*") == 0 && s_.compare(i_, 3, "(*)") != 0) { block("*)"); }
      else if (c == '`') { while (i_ < s_.size() && s_[i_] != '\n') ++i_; }  // compiler directive
      else break;
    }
  }
  void block(const char* close) {
    const auto end = s_.find(close, i_ + 2);
    if (end == std::string::npos) throw ParseError(file_, line_, std::string("unterminated comment or attribute"));
    for (std::size_t k = i_; k < end; ++k) line_ += s_[k] == '\n';
    i_ = end + 2;
  }

  std::string s_;
  std::string file_;
  std::size_t i_ = 0;
  int line_ = 1;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
const std::map<std::string, GateType> kPrimitives = {
    {"and", GateType::And}, {"nand", GateType::Nand}, {"or", GateType::Or},   {"nor", GateType::Nor},
    {"xor", GateType::Xor}, {"xnor", GateType::Xnor}, {"not", GateType::Not}, {"buf", GateType::Buf},
};

const std::set<std::string> kHelperModules = {"rith_dff", "rith_mux2"};

struct Range { long msb = 0, lsb = 0; };

class Parser {
 public:
  Parser(std::vector<Tok> toks, std::string file) : t_(std::move(toks)), file_(std::move(file)) {}

  Netlist run() {
    bool have_top = false;
    while (peek().kind != Tok::End) {
      expect_word("module");
      const std::string name = ident("module name");
      if (kHelperModules.count(name)) { skip_module(); continue; }
      if (have_top)
        fail("more than one module ('" + name + "'); hierarchical netlists are not supported yet, flatten first");
      have_top = true;
      nl_.name = name;
      module_body();
    }
    if (!have_top) fail("no module found");
    for (const auto& bit : output_bits_) {
      const GateId d = nl_.find(bit);
      if (d == kNoGate) fail("output '" + bit + "' is never driven");
      nl_.add_output(bit, d);
    }
    try {
      nl_.finalize();
    } catch (const NetlistError& e) {
      throw ParseError(file_, t_.back().line, e.what());
    }
    return std::move(nl_);
  }

 private:
  // -- token helpers --
  const Tok& peek(std::size_t k = 0) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
  const Tok& next() { const Tok& t = peek(); if (p_ < t_.size() - 1) ++p_; return t; }
  bool is_sym(const char* s) const { return peek().kind == Tok::Sym && peek().text == s; }
  bool is_word(const char* s) const { return peek().kind == Tok::Ident && peek().text == s; }
  [[noreturn]] void fail(const std::string& msg) const { throw ParseError(file_, peek().line, msg); }
  void expect_sym(const char* s) {
    if (!is_sym(s)) fail(std::string("expected '") + s + "' but found '" + peek().text + "'");
    next();
  }
  void expect_word(const char* s) {
    if (!is_word(s)) fail(std::string("expected '") + s + "' but found '" + peek().text + "'");
    next();
  }
  std::string ident(const char* what) {
    if (peek().kind != Tok::Ident) fail(std::string("expected ") + what + " but found '" + peek().text + "'");
    return next().text;
  }
  long number() {
    if (peek().kind != Tok::Number) fail("expected a number but found '" + peek().text + "'");
    return std::stol(next().text);
  }

  void skip_module() {
    while (!is_word("endmodule")) {
      if (peek().kind == Tok::End) fail("missing endmodule");
      next();
    }
    next();
  }

  // -- declarations --
  std::optional<Range> opt_range() {
    if (!is_sym("[")) return std::nullopt;
    next();
    Range r;
    r.msb = number();
    expect_sym(":");
    r.lsb = number();
    expect_sym("]");
    return r;
  }

  std::vector<std::string> bits_of(const std::string& name) const {
    auto it = ranges_.find(name);
    if (it == ranges_.end()) return {name};
    std::vector<std::string> bits;
    const auto [msb, lsb] = it->second;
    const long step = msb >= lsb ? -1 : 1;
    for (long i = msb;; i += step) {
      bits.push_back(name + "[" + std::to_string(i) + "]");
      if (i == lsb) break;
    }
    return bits;
  }

  void declare(const std::string& kind, const std::string& name, const std::optional<Range>& r) {
    if (r) ranges_[name] = *r;
    if (kind == "input") {
      for (const auto& b : bits_of(name)) {
        try { nl_.define(nl_.get_or_declare(b), GateType::Input); }
        catch (const NetlistError& e) { fail(e.what()); }
      }
    } else if (kind == "output") {
      for (const auto& b : bits_of(name)) output_bits_.push_back(b);
    } else if (kind == "inout") {
      fail("inout port '" + name + "' is not supported");
    }
  }

  // Parses "input [3:0] a, b" up to (not including) ';' or ')' of an ANSI header.
  void declaration(const std::string& kind, bool in_header) {
    while (is_word("wire") || is_word("reg") || is_word("signed") || is_word("logic")) next();
    const auto r = opt_range();
    while (true) {
      declare(kind, ident("a name"), r);
      if (is_sym("=")) fail("initialised declarations are not supported");
      if (!is_sym(",")) break;
      if (in_header && peek(1).kind == Tok::Ident &&
          (peek(1).text == "input" || peek(1).text == "output" || peek(1).text == "inout"))
        break;
      next();
    }
  }

  void module_body() {
    if (is_sym("#")) fail("parameterised modules are not supported");
    if (is_sym("(")) {
      next();
      while (!is_sym(")")) {
        if (is_word("input") || is_word("output") || is_word("inout")) {
          const std::string kind = next().text;
          declaration(kind, true);
        } else {
          ident("a port name");  // non-ANSI: direction comes later
        }
        if (is_sym(",")) next();
        else if (!is_sym(")")) fail("expected ',' or ')' in the port list");
      }
      next();
    }
    expect_sym(";");

    while (!is_word("endmodule")) {
      if (peek().kind == Tok::End) fail("missing endmodule");
      const std::string kw = ident("a module item");
      if (kw == "input" || kw == "output" || kw == "inout" || kw == "wire" || kw == "reg" ||
          kw == "supply0" || kw == "supply1" || kw == "tri" || kw == "logic") {
        if (kw == "supply0" || kw == "supply1") fail("supply nets are not supported yet");
        declaration(kw, false);
        expect_sym(";");
      } else if (kw == "assign") {
        assign_stmt();
      } else if (kPrimitives.count(kw)) {
        primitive(kPrimitives.at(kw));
      } else if (kw == "always" || kw == "initial" || kw == "function" || kw == "task" ||
                 kw == "parameter" || kw == "localparam" || kw == "generate") {
        fail("'" + kw + "' is behavioural; only gate-level netlists are supported");
      } else {
        cell(kw);
      }
    }
    next();
  }

  // -- nets --
  // Value of a 1-bit constant such as 0, 1'b1 or 1'h0.
  bool const_value(const std::string& text) {
    const auto q = text.find('\'');
    std::size_t digits_at = 0;
    if (q != std::string::npos) {
      digits_at = q + 2;
      if (q + 1 < text.size() && (text[q + 1] == 's' || text[q + 1] == 'S')) ++digits_at;
    }
    const long width = (q == std::string::npos || q == 0) ? 1 : std::stol(text.substr(0, q));
    if (width != 1) fail("multi-bit constant '" + text + "' is not supported here");
    bool one = false;
    for (std::size_t k = digits_at; k < text.size(); ++k) {
      const char c = text[k];
      if (c == 'x' || c == 'X' || c == 'z' || c == 'Z' || c == '?')
        fail("x/z constant '" + text + "' is not supported");
      if (c != '0' && c != '_') one = true;
    }
    return one;
  }

  // A shared constant net for a constant used as a gate input.
  GateId constant(const std::string& text) {
    const bool one = const_value(text);
    const char* name = one ? "rith_const1" : "rith_const0";
    const GateId id = nl_.get_or_declare(name);
    if (nl_.gate(id).type == GateType::Undefined) nl_.define(id, one ? GateType::Const1 : GateType::Const0);
    return id;
  }

  std::string net_name() {
    const std::string base = ident("a net name");
    if (is_sym("[")) {
      next();
      const long idx = number();
      if (is_sym(":")) fail("part-selects like '" + base + "[a:b]' are not supported");
      expect_sym("]");
      return base + "[" + std::to_string(idx) + "]";
    }
    auto it = ranges_.find(base);
    if (it != ranges_.end()) {
      if (it->second.msb != it->second.lsb)
        fail("whole-bus connection of '" + base + "' is not supported; connect bits individually");
      return base + "[" + std::to_string(it->second.msb) + "]";
    }
    return base;
  }

  GateId net_ref() {
    if (is_sym("{")) fail("concatenations are not supported yet");
    if (peek().kind == Tok::Number) return constant(next().text);
    return nl_.get_or_declare(net_name());
  }

  void drive(GateId out, GateType type, std::vector<GateId> fanins) {
    try { nl_.define(out, type, std::move(fanins)); }
    catch (const NetlistError& e) { fail(e.what()); }
  }

  void assign_stmt() {
    while (true) {
      const GateId lhs = nl_.get_or_declare(net_name());
      expect_sym("=");
      if (is_sym("~")) {
        next();
        drive(lhs, GateType::Not, {net_ref()});
      } else if (peek().kind == Tok::Number) {
        drive(lhs, const_value(next().text) ? GateType::Const1 : GateType::Const0, {});
      } else {
        drive(lhs, GateType::Buf, {net_ref()});
      }
      if (!is_sym(",")) break;
      next();
    }
    if (!is_sym(";")) fail("only 'assign net = net | ~net | constant;' is supported");
    next();
  }

  void primitive(GateType type) {
    if (is_sym("#")) fail("delays on primitives are not supported");
    if (peek().kind == Tok::Ident) next();  // optional instance name
    expect_sym("(");
    const GateId out = nl_.get_or_declare(net_name());
    std::vector<GateId> ins;
    while (is_sym(",")) {
      next();
      ins.push_back(net_ref());
    }
    expect_sym(")");
    expect_sym(";");
    drive(out, type, std::move(ins));
  }

  void cell(const std::string& type) {
    if (is_sym("#")) fail("parameterised instance of '" + type + "' is not supported");
    const std::string inst = peek().kind == Tok::Ident ? next().text : type + "_" + std::to_string(++anon_);
    expect_sym("(");
    std::map<std::string, std::string> pins;  // pin -> net name ("" when unconnected)
    std::map<std::string, GateId> const_pin;
    while (!is_sym(")")) {
      if (!is_sym(".")) fail("instance '" + inst + "' of '" + type + "' must use named port connections");
      next();
      const std::string pin = ident("a pin name");
      expect_sym("(");
      if (is_sym(")")) {
        pins[pin] = "";
      } else if (peek().kind == Tok::Number) {
        const_pin[pin] = constant(next().text);
        pins[pin] = nl_.gate(const_pin[pin]).name;
      } else {
        if (is_sym("{")) fail("concatenations are not supported yet");
        pins[pin] = net_name();
      }
      expect_sym(")");
      if (is_sym(",")) next();
    }
    next();
    expect_sym(";");

    auto in = [&](const char* pin) -> GateId {
      auto it = pins.find(pin);
      if (it == pins.end() || it->second.empty())
        fail("instance '" + inst + "' of '" + type + "' has no connection on input " + pin);
      return nl_.get_or_declare(it->second);
    };
    auto out = [&](const char* pin) -> GateId {
      auto it = pins.find(pin);
      const std::string net = (it == pins.end() || it->second.empty()) ? inst + "/" + pin : it->second;
      return nl_.get_or_declare(net);
    };
    auto two_input = [&](GateType t) { drive(out("Y"), t, {in("A"), in("B")}); };

    if (type == "$_BUF_") drive(out("Y"), GateType::Buf, {in("A")});
    else if (type == "$_NOT_") drive(out("Y"), GateType::Not, {in("A")});
    else if (type == "$_AND_") two_input(GateType::And);
    else if (type == "$_NAND_") two_input(GateType::Nand);
    else if (type == "$_OR_") two_input(GateType::Or);
    else if (type == "$_NOR_") two_input(GateType::Nor);
    else if (type == "$_XOR_") two_input(GateType::Xor);
    else if (type == "$_XNOR_") two_input(GateType::Xnor);
    else if (type == "$_ANDNOT_" || type == "$_ORNOT_") {
      const GateId nb = nl_.get_or_declare(inst + "/B_n");
      drive(nb, GateType::Not, {in("B")});
      drive(out("Y"), type == "$_ANDNOT_" ? GateType::And : GateType::Or, {in("A"), nb});
    } else if (type == "$_MUX_" || type == "rith_mux2") {
      drive(out("Y"), GateType::Mux2, {in("S"), in("A"), in("B")});
    } else if (type == "$_DFF_P_" || type == "rith_dff") {
      const GateId q = out("Q");
      drive(q, GateType::Dff, {in("D")});
      nl_.set_clock(q, in("C"));
    } else if (type.rfind("$_DFF", 0) == 0 || type.rfind("$_SDFF", 0) == 0 || type.rfind("$_DLATCH", 0) == 0) {
      fail("Yosys cell '" + type + "' (negative edge, reset, enable or latch) is not supported yet");
    } else {
      fail("unknown cell '" + type + "' (instance '" + inst +
           "'); Liberty cell mapping is not supported yet");
    }
  }

  std::vector<Tok> t_;
  std::string file_;
  std::size_t p_ = 0;
  Netlist nl_;
  std::map<std::string, Range> ranges_;
  std::vector<std::string> output_bits_;
  int anon_ = 0;
};

const std::unordered_set<std::string> kKeywords = {
    "always", "and", "assign", "begin", "buf", "case", "default", "else", "end", "endcase",
    "endmodule", "for", "function", "if", "initial", "inout", "input", "integer", "module",
    "nand", "negedge", "nor", "not", "or", "output", "parameter", "posedge", "reg", "supply0",
    "supply1", "task", "tri", "wire", "xnor", "xor", "logic", "generate", "localparam"};

}  // namespace

std::string verilog_identifier(std::string_view name) {
  bool simple = !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
  for (char c : name)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$')) simple = false;
  if (simple && !kKeywords.count(std::string(name))) return std::string(name);
  return "\\" + std::string(name) + " ";
}

Netlist read_verilog(std::istream& in, const std::string& filename) {
  std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return Parser(Lexer(std::move(src), filename).run(), filename).run();
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
void write_verilog(std::ostream& out, const Netlist& nl) {
  const auto id = [&](GateId g) { return verilog_identifier(nl.gate(g).name); };

  std::set<std::string> output_names;
  for (const auto& [name, d] : nl.outputs()) {
    if (nl.find(name) != kNoGate && nl.gate(nl.find(name)).type == GateType::Input)
      throw NetlistError("output '" + name + "' has the same name as an input");
    output_names.insert(name);
  }
  bool need_clk = false, uses_dff = false, uses_mux = false;
  for (GateId f : nl.flops()) {
    uses_dff = true;
    if (nl.gate(f).clock == kNoGate) need_clk = true;
  }
  uses_mux = nl.count(GateType::Mux2) > 0;
  const std::string clk = "rith_clk";
  if (need_clk && nl.find(clk) != kNoGate) throw NetlistError("net name 'rith_clk' is reserved");

  out << "// written by rithereal\n";
  out << "module " << verilog_identifier(nl.name) << " (";
  bool first = true;
  auto port = [&](const std::string& n) { out << (first ? "" : ",") << "\n    " << n; first = false; };
  if (need_clk) port(clk);
  for (GateId g : nl.inputs()) port(id(g));
  for (const auto& [name, d] : nl.outputs()) port(verilog_identifier(name));
  out << "\n);\n";

  if (need_clk) out << "  input " << clk << ";\n";
  for (GateId g : nl.inputs()) out << "  input " << id(g) << ";\n";
  for (const auto& [name, d] : nl.outputs()) out << "  output " << verilog_identifier(name) << ";\n";
  for (GateId g : nl.definition_order())
    if (nl.gate(g).type != GateType::Input && !output_names.count(nl.gate(g).name))
      out << "  wire " << id(g) << ";\n";
  out << "\n";

  std::size_t inst_no = 0;
  for (GateId g : nl.definition_order()) {
    const Gate& gate = nl.gate(g);
    const auto fi = nl.fanins(g);
    if (gate.type == GateType::Input) continue;
    const std::string inst = "rith_i" + std::to_string(inst_no++);
    switch (gate.type) {
      case GateType::Input: break;
      case GateType::Const0: out << "  assign " << id(g) << " = 1'b0;\n"; break;
      case GateType::Const1: out << "  assign " << id(g) << " = 1'b1;\n"; break;
      case GateType::Mux2:
        out << "  rith_mux2 " << inst << " (.S(" << id(fi[0]) << "), .A(" << id(fi[1]) << "), .B("
            << id(fi[2]) << "), .Y(" << id(g) << "));\n";
        break;
      case GateType::Dff:
        out << "  rith_dff " << inst << " (.C(" << (gate.clock == kNoGate ? clk : id(gate.clock))
            << "), .D(" << id(fi[0]) << "), .Q(" << id(g) << "));\n";
        break;
      default: {
        std::string prim(to_string(gate.type));
        for (auto& c : prim) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out << "  " << prim << " " << inst << " (" << id(g);
        for (GateId f : fi) out << ", " << id(f);
        out << ");\n";
      }
    }
  }
  for (const auto& [name, d] : nl.outputs())
    if (name != nl.gate(d).name) out << "  assign " << verilog_identifier(name) << " = " << id(d) << ";\n";
  out << "endmodule\n";

  if (uses_dff)
    out << "\nmodule rith_dff (input C, input D, output reg Q);\n"
           "  always @(posedge C) Q <= D;\nendmodule\n";
  if (uses_mux)
    out << "\nmodule rith_mux2 (input S, input A, input B, output Y);\n"
           "  assign Y = S ? B : A;\nendmodule\n";
}

}  // namespace rith
