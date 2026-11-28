// SPDX-License-Identifier: Apache-2.0
// Structural Verilog: parser into a Design, writer, and elaboration to gates.
#include "rith/design.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <istream>
#include <iterator>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "rith/io.hpp"

namespace rith {

// ---------------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------------
std::vector<long> BitRange::indices() const {
  std::vector<long> v;
  const long step = msb >= lsb ? -1 : 1;
  for (long i = msb;; i += step) {
    v.push_back(i);
    if (i == lsb) break;
  }
  return v;
}

NetId Instance::net_on(std::string_view pin) const {
  for (const auto& c : conns)
    if (c.pin == pin) return c.net;
  return kNoNet;
}

void Instance::connect(std::string_view pin, NetId n) {
  for (auto& c : conns)
    if (c.pin == pin) {
      c.net = n;
      return;
    }
  conns.push_back({std::string(pin), n});
}

std::size_t Design::add_instance(Instance inst) {
  instances.push_back(std::move(inst));
  order_.emplace_back(false, instances.size() - 1);
  return instances.size() - 1;
}

std::size_t Design::add_assign(Assign a) {
  assigns.push_back(a);
  order_.emplace_back(true, assigns.size() - 1);
  return assigns.size() - 1;
}

NetId Design::net(std::string_view n) {
  auto it = index_.find(std::string(n));
  if (it != index_.end()) return it->second;
  const auto id = static_cast<NetId>(nets_.size());
  nets_.emplace_back(n);
  index_.emplace(std::string(n), id);
  return id;
}

NetId Design::find_net(std::string_view n) const {
  auto it = index_.find(std::string(n));
  return it == index_.end() ? kNoNet : it->second;
}

NetId Design::fresh_net(std::string_view prefix) {
  std::string base(prefix);
  if (find_net(base) == kNoNet && !buses.count(base)) return net(base);
  for (std::size_t k = 0;; ++k) {
    std::string n = base + "_" + std::to_string(k);
    if (find_net(n) == kNoNet && !buses.count(n)) return net(n);
  }
}

std::string Design::fresh_instance_name(std::string_view prefix) const {
  std::set<std::string> used;
  for (const auto& i : instances) used.insert(i.name);
  std::string base(prefix);
  if (!used.count(base)) return base;
  for (std::size_t k = 0;; ++k)
    if (!used.count(base + "_" + std::to_string(k))) return base + "_" + std::to_string(k);
}

std::vector<std::string> Design::port_bits(const PortDecl& p) const {
  if (!p.range) return {p.name};
  std::vector<std::string> bits;
  for (long i : p.range->indices()) bits.push_back(p.name + "[" + std::to_string(i) + "]");
  return bits;
}

const PortDecl* Design::find_port(std::string_view n) const {
  for (const auto& p : ports)
    if (p.name == n) return &p;
  return nullptr;
}

NetId Design::add_port(std::string port, PortDir dir) {
  if (find_port(port)) throw NetlistError("port '" + port + "' already exists");
  const NetId n = net(port);
  ports.push_back({std::move(port), dir, std::nullopt});
  return n;
}

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
// Parser: tokens -> Design. No cell knowledge here; elaborate() checks cells.
// ---------------------------------------------------------------------------
const std::set<std::string> kPrimitiveNames = {"and", "nand", "or", "nor", "xor", "xnor", "not", "buf"};
const std::set<std::string> kHelperModules = {"rith_dff", "rith_dffn", "rith_mux2"};

class Parser {
 public:
  Parser(std::vector<Tok> toks, std::string file) : t_(std::move(toks)), file_(std::move(file)) {}

  Design run() {
    bool have_top = false;
    while (peek().kind != Tok::End) {
      expect_word("module");
      const std::string name = ident("module name");
      if (kHelperModules.count(name)) { skip_module(); continue; }
      if (have_top)
        fail("more than one module ('" + name + "'); hierarchical netlists are not supported yet, flatten first");
      have_top = true;
      d_.name = name;
      module_body();
    }
    if (!have_top) fail("no module found");
    // Ports in header order; non-ANSI ports get their direction from the body.
    std::vector<PortDecl> ordered;
    for (const auto& n : header_) {
      auto it = decl_.find(n);
      if (it == decl_.end()) throw ParseError(file_, header_line_, "port '" + n + "' has no direction declaration");
      ordered.push_back(it->second);
    }
    for (const auto& n : body_ports_)  // declared in the body but missing from the header: keep them
      if (std::find(header_.begin(), header_.end(), n) == header_.end()) ordered.push_back(decl_.at(n));
    d_.ports = std::move(ordered);
    return std::move(d_);
  }

 private:
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

  std::optional<BitRange> opt_range() {
    if (!is_sym("[")) return std::nullopt;
    next();
    BitRange r;
    r.msb = number();
    expect_sym(":");
    r.lsb = number();
    expect_sym("]");
    return r;
  }

  void declare(const std::string& kind, const std::string& name, const std::optional<BitRange>& r, bool in_header) {
    if (r) d_.buses[name] = *r;
    if (kind == "inout") fail("inout port '" + name + "' is not supported");
    if (kind == "input" || kind == "output") {
      if (decl_.count(name)) fail("port '" + name + "' is declared twice");
      decl_[name] = PortDecl{name, kind == "input" ? PortDir::Input : PortDir::Output, r};
      if (in_header) header_.push_back(name);
      else body_ports_.push_back(name);
      for (const auto& b : d_.port_bits(decl_[name])) d_.net(b);
    }
  }

  void declaration(const std::string& kind, bool in_header) {
    while (is_word("wire") || is_word("reg") || is_word("signed") || is_word("logic")) next();
    const auto r = opt_range();
    while (true) {
      declare(kind, ident("a name"), r, in_header);
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
    header_line_ = peek().line;
    if (is_sym("(")) {
      next();
      while (!is_sym(")")) {
        if (is_word("input") || is_word("output") || is_word("inout")) {
          const std::string kind = next().text;
          declaration(kind, true);
        } else {
          header_.push_back(ident("a port name"));  // non-ANSI: direction comes later
        }
        if (is_sym(",")) next();
        else if (!is_sym(")")) fail("expected ',' or ')' in the port list");
      }
      next();
    }
    expect_sym(";");

    while (!is_word("endmodule")) {
      if (peek().kind == Tok::End) fail("missing endmodule");
      const int line = peek().line;
      const std::string kw = ident("a module item");
      if (kw == "input" || kw == "output" || kw == "inout" || kw == "wire" || kw == "reg" ||
          kw == "supply0" || kw == "supply1" || kw == "tri" || kw == "logic") {
        if (kw == "supply0" || kw == "supply1") fail("supply nets are not supported yet");
        declaration(kw, false);
        expect_sym(";");
      } else if (kw == "assign") {
        assign_stmt(line);
      } else if (kPrimitiveNames.count(kw)) {
        primitive(kw, line);
      } else if (kw == "always" || kw == "initial" || kw == "function" || kw == "task" ||
                 kw == "parameter" || kw == "localparam" || kw == "generate") {
        fail("'" + kw + "' is behavioural; only gate-level netlists are supported");
      } else {
        cell(kw, line);
      }
    }
    next();
  }

  // Value of a 1-bit constant such as 0, 1'b1 or 1'h0.
  NetId constant(const std::string& text) {
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
    return one ? kConst1Net : kConst0Net;
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
    auto it = d_.buses.find(base);
    if (it != d_.buses.end()) {
      if (it->second.msb != it->second.lsb)
        fail("whole-bus connection of '" + base + "' is not supported; connect bits individually");
      return base + "[" + std::to_string(it->second.msb) + "]";
    }
    return base;
  }

  NetId net_ref() {
    if (is_sym("{")) fail("concatenations are not supported yet");
    if (peek().kind == Tok::Number) return constant(next().text);
    return d_.net(net_name());
  }

  void assign_stmt(int line) {
    while (true) {
      Assign a;
      a.line = line;
      a.lhs = d_.net(net_name());
      expect_sym("=");
      if (is_sym("~")) {
        next();
        a.invert = true;
      }
      a.rhs = net_ref();
      d_.add_assign(a);
      if (!is_sym(",")) break;
      next();
    }
    if (!is_sym(";")) fail("only 'assign net = net | ~net | constant;' is supported");
    next();
  }

  void primitive(const std::string& type, int line) {
    if (is_sym("#")) fail("delays on primitives are not supported");
    Instance inst;
    inst.type = type;
    inst.positional = true;
    inst.line = line;
    if (peek().kind == Tok::Ident) inst.name = next().text;  // optional instance name
    expect_sym("(");
    inst.conns.push_back({"", d_.net(net_name())});
    while (is_sym(",")) {
      next();
      inst.conns.push_back({"", net_ref()});
    }
    expect_sym(")");
    expect_sym(";");
    d_.add_instance(std::move(inst));
  }

  void cell(const std::string& type, int line) {
    if (is_sym("#")) fail("parameterised instance of '" + type + "' is not supported");
    Instance inst;
    inst.type = type;
    inst.line = line;
    inst.name = peek().kind == Tok::Ident ? next().text : type + "_" + std::to_string(++anon_);
    expect_sym("(");
    while (!is_sym(")")) {
      if (!is_sym(".")) fail("instance '" + inst.name + "' of '" + type + "' must use named port connections");
      next();
      PinConn c;
      c.pin = ident("a pin name");
      expect_sym("(");
      if (!is_sym(")")) c.net = net_ref();
      expect_sym(")");
      inst.conns.push_back(std::move(c));
      if (is_sym(",")) next();
    }
    next();
    expect_sym(";");
    d_.add_instance(std::move(inst));
  }

  std::vector<Tok> t_;
  std::string file_;
  std::size_t p_ = 0;
  Design d_;
  std::vector<std::string> header_, body_ports_;
  std::map<std::string, PortDecl> decl_;
  int header_line_ = 0;
  int anon_ = 0;
};

// ---------------------------------------------------------------------------
// Elaboration: Design -> Netlist of primitive gates
// ---------------------------------------------------------------------------
std::string trim_copy(const std::string& s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

const std::map<std::string, GateType> kPrimitives = {
    {"and", GateType::And}, {"nand", GateType::Nand}, {"or", GateType::Or},   {"nor", GateType::Nor},
    {"xor", GateType::Xor}, {"xnor", GateType::Xnor}, {"not", GateType::Not}, {"buf", GateType::Buf},
};

class Elaborator {
 public:
  Elaborator(const Design& d, const Library* lib, std::string file) : d_(d), lib_(lib), file_(std::move(file)) {}

  Elaboration run() {
    nl_.name = d_.name;
    flop_of_.assign(d_.instances.size(), kNoGate);
    for (const auto& p : d_.ports)
      if (p.dir == PortDir::Input)
        for (const auto& b : d_.port_bits(p)) {
          try { nl_.define(nl_.get_or_declare(b), GateType::Input); }
          catch (const NetlistError& e) { throw ParseError(file_, 0, e.what()); }
        }
    for (const auto& [is_assign, i] : d_.order()) {
      if (is_assign) assign(d_.assigns[i]);
      else instance(d_.instances[i], i);
    }
    for (const auto& p : d_.ports)
      if (p.dir == PortDir::Output)
        for (const auto& b : d_.port_bits(p)) {
          const GateId g = nl_.find(b);
          if (g == kNoGate) throw ParseError(file_, 0, "output '" + b + "' is never driven");
          nl_.add_output(b, g);
        }
    if (async_ignored_)
      nl_.warnings.push_back(std::to_string(async_ignored_) +
                             " flop(s) have asynchronous set/reset; they are assumed inactive in test mode");
    if (latches_)
      nl_.warnings.push_back(std::to_string(latches_) +
                             " latch(es) modeled as transparent (output follows data_in), as for lockup latches");
    try {
      nl_.finalize();
    } catch (const NetlistError& e) {
      throw ParseError(file_, 0, e.what());
    }
    return {std::move(nl_), std::move(flop_of_)};
  }

 private:
  [[noreturn]] void fail(const std::string& msg) const { throw ParseError(file_, line_, msg); }

  // Gate driving a net (a constant net becomes a shared constant gate).
  GateId g(NetId n) {
    if (n == kConst0Net || n == kConst1Net) {
      const bool one = n == kConst1Net;
      const GateId id = nl_.get_or_declare(one ? "rith_const1" : "rith_const0");
      if (nl_.gate(id).type == GateType::Undefined) nl_.define(id, one ? GateType::Const1 : GateType::Const0);
      return id;
    }
    return nl_.get_or_declare(d_.net_name(n));
  }

  void drive(GateId out, GateType type, std::vector<GateId> fanins) {
    try { nl_.define(out, type, std::move(fanins)); }
    catch (const NetlistError& e) { fail(e.what()); }
  }

  void assign(const Assign& a) {
    line_ = a.line;
    const GateId lhs = g(a.lhs);
    if (is_constant(a.rhs)) {
      const bool one = (a.rhs == kConst1Net) != a.invert;
      drive(lhs, one ? GateType::Const1 : GateType::Const0, {});
    } else {
      drive(lhs, a.invert ? GateType::Not : GateType::Buf, {g(a.rhs)});
    }
  }

  void instance(const Instance& inst, std::size_t idx) {
    line_ = inst.line;
    if (inst.positional) {
      auto it = kPrimitives.find(inst.type);
      if (it == kPrimitives.end()) fail("unknown primitive '" + inst.type + "'");
      if (is_constant(inst.conns[0].net)) fail("primitive output connected to a constant");
      std::vector<GateId> ins;
      for (std::size_t k = 1; k < inst.conns.size(); ++k) ins.push_back(g(inst.conns[k].net));
      drive(g(inst.conns[0].net), it->second, std::move(ins));
      return;
    }
    const std::string& type = inst.type;
    auto in = [&](const char* pin) -> GateId {
      const NetId n = inst.net_on(pin);
      if (n == kNoNet) fail("instance '" + inst.name + "' of '" + type + "' has no connection on input " + pin);
      return g(n);
    };
    auto out = [&](const char* pin) -> GateId {
      const NetId n = inst.net_on(pin);
      return n == kNoNet ? nl_.get_or_declare(inst.name + "/" + pin) : g(n);
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
      const GateId nb = nl_.get_or_declare(inst.name + "/B_n");
      drive(nb, GateType::Not, {in("B")});
      drive(out("Y"), type == "$_ANDNOT_" ? GateType::And : GateType::Or, {in("A"), nb});
    } else if (type == "$_MUX_" || type == "rith_mux2") {
      drive(out("Y"), GateType::Mux2, {in("S"), in("A"), in("B")});
    } else if (type == "$_DFF_P_" || type == "rith_dff" || type == "$_DFF_N_" || type == "rith_dffn") {
      const GateId q = out("Q");
      drive(q, GateType::Dff, {in("D")});
      nl_.set_clock(q, in("C"), type == "$_DFF_N_" || type == "rith_dffn");
      flop_of_[idx] = q;
    } else if (type.rfind("$_DFF", 0) == 0 || type.rfind("$_SDFF", 0) == 0 || type.rfind("$_DLATCH", 0) == 0) {
      fail("Yosys cell '" + type + "' (negative edge, reset, enable or latch) is not supported yet");
    } else if (const LibCell* lc = lib_ ? lib_->find(type) : nullptr) {
      library_cell(*lc, inst, idx);
    } else {
      fail("unknown cell '" + type + "' (instance '" + inst.name + "')" +
           (lib_ ? std::string(" is not in the Liberty library") : std::string("; pass its Liberty library")));
    }
  }

  // Expands a Liberty cell instance into primitive gates.
  void library_cell(const LibCell& lc, const Instance& inst, std::size_t idx) {
    const std::string& name = inst.name;
    const LibLatch* latch = lc.latch ? &*lc.latch : nullptr;
    if (lc.has_latch && (!latch || latch->data_in.empty() || latch->iq.empty()))
      fail("latch cell '" + lc.name + "' (instance '" + name + "') has no data_in; not supported");
    if (lc.has_statetable)
      fail("cell '" + lc.name + "' (instance '" + name + "') uses a statetable (e.g. a clock gate); not supported yet");
    for (const auto& c : inst.conns)
      if (!lc.pin(c.pin)) fail("cell '" + lc.name + "' has no pin '" + c.pin + "' (instance '" + name + "')");

    std::size_t tmp = 0;
    auto fresh = [&] { return nl_.get_or_declare(name + "/$" + std::to_string(tmp++)); };
    auto net_of_output = [&](const LibPin& p) -> GateId {
      const NetId n = inst.net_on(p.name);
      if (n == kNoNet) return kNoGate;  // unconnected output
      if (is_constant(n)) fail("output pin " + p.name + " of instance '" + name + "' is tied to a constant");
      return g(n);
    };

    GateId flop = kNoGate, flop_n = kNoGate;
    GateId latch_q = kNoGate, latch_qn = kNoGate;  // a transparent latch's state, built on first use
    const LibFlop* ff = lc.flop ? &*lc.flop : nullptr;
    if (ff) {
      for (const auto& p : lc.pins)  // name the flop after the net its Q pin drives
        if (p.dir == LibPin::Dir::Output && trim_copy(p.function) == ff->iq) {
          flop = net_of_output(p);
          if (flop != kNoGate) break;
        }
      if (flop == kNoGate) flop = nl_.get_or_declare(name + "/" + ff->iq);
    }

    auto parse_fn = [&](const std::string& text) {
      try { return parse_liberty_function(text); }
      catch (const std::exception& ex) { fail("cell '" + lc.name + "': " + ex.what()); }
    };

    std::function<GateId(const LibExpr&, GateId)> build = [&](const LibExpr& e, GateId out) -> GateId {
      auto place = [&](GateType t, std::vector<GateId> fi) {
        const GateId gid = out != kNoGate ? out : fresh();
        drive(gid, t, std::move(fi));
        return gid;
      };
      switch (e.op) {
        case LibExpr::Op::Const0: return place(GateType::Const0, {});
        case LibExpr::Op::Const1: return place(GateType::Const1, {});
        case LibExpr::Op::Var: {
          GateId src;
          if (latch && (e.var == latch->iq || (!latch->iqn.empty() && e.var == latch->iqn))) {
            if (latch_q == kNoGate) latch_q = build(parse_fn(latch->data_in), kNoGate);
            if (e.var == latch->iq) src = latch_q;
            else {
              if (latch_qn == kNoGate) {
                latch_qn = nl_.get_or_declare(name + "/" + latch->iqn);
                drive(latch_qn, GateType::Not, {latch_q});
              }
              src = latch_qn;
            }
          } else if (ff && e.var == ff->iq) src = flop;
          else if (ff && !ff->iqn.empty() && e.var == ff->iqn) {
            if (flop_n == kNoGate) {
              flop_n = nl_.get_or_declare(name + "/" + ff->iqn);
              drive(flop_n, GateType::Not, {flop});
            }
            src = flop_n;
          } else {
            const LibPin* p = lc.pin(e.var);
            if (!p || p->dir == LibPin::Dir::Output)
              fail("function of cell '" + lc.name + "' uses unknown input '" + e.var + "'");
            const NetId n = inst.net_on(e.var);
            if (n == kNoNet)
              fail("instance '" + name + "' of '" + lc.name + "' has no connection on input " + e.var);
            src = g(n);
          }
          return out == kNoGate ? src : place(GateType::Buf, {src});
        }
        case LibExpr::Op::Not: {
          const LibExpr& a = e.args[0];
          if (a.op == LibExpr::Op::And || a.op == LibExpr::Op::Or || a.op == LibExpr::Op::Xor) {
            std::vector<GateId> fi;
            for (const auto& x : a.args) fi.push_back(build(x, kNoGate));
            return place(a.op == LibExpr::Op::And ? GateType::Nand
                         : a.op == LibExpr::Op::Or ? GateType::Nor : GateType::Xnor, std::move(fi));
          }
          return place(GateType::Not, {build(a, kNoGate)});
        }
        case LibExpr::Op::And:
        case LibExpr::Op::Or:
        case LibExpr::Op::Xor: {
          std::vector<GateId> fi;
          for (const auto& x : e.args) fi.push_back(build(x, kNoGate));
          return place(e.op == LibExpr::Op::And ? GateType::And
                       : e.op == LibExpr::Op::Or ? GateType::Or : GateType::Xor, std::move(fi));
        }
      }
      return kNoGate;
    };

    if (latch) ++latches_;
    if (ff) {
      if (ff->next_state.empty() || ff->clocked_on.empty())
        fail("flop cell '" + lc.name + "' lacks next_state or clocked_on");
      const GateId dn = build(parse_fn(ff->next_state), kNoGate);
      drive(flop, GateType::Dff, {dn});
      const LibExpr clk = parse_fn(ff->clocked_on);
      const bool falling = clk.op == LibExpr::Op::Not && clk.args[0].op == LibExpr::Op::Var;
      const LibExpr& clk_pin = falling ? clk.args[0] : clk;
      if (clk_pin.op != LibExpr::Op::Var)
        fail("flop cell '" + lc.name + "' has clocked_on \"" + ff->clocked_on +
             "\"; only a single clock pin (either edge) is supported");
      const NetId cn = inst.net_on(clk_pin.var);
      if (cn != kNoNet) nl_.set_clock(flop, g(cn), falling);
      if (!ff->clear.empty() || !ff->preset.empty()) ++async_ignored_;
      flop_of_[idx] = flop;
    }
    for (const auto& p : lc.pins) {
      if (p.dir != LibPin::Dir::Output) continue;
      if (p.function.empty()) {
        if (net_of_output(p) != kNoGate) fail("output pin " + p.name + " of cell '" + lc.name + "' has no function");
        continue;
      }
      const GateId o = net_of_output(p);
      if (o == kNoGate) continue;
      if (o == flop) continue;  // Q pin already is the flop
      build(parse_fn(p.function), o);
    }
  }

  const Design& d_;
  const Library* lib_;
  std::string file_;
  int line_ = 0;
  Netlist nl_;
  std::vector<GateId> flop_of_;
  std::size_t async_ignored_ = 0;
  std::size_t latches_ = 0;
};

}  // namespace

Design read_verilog_design(std::istream& in, const std::string& filename) {
  std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return Parser(Lexer(std::move(src), filename).run(), filename).run();
}

Design read_verilog_design_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open '" + path + "'");
  return read_verilog_design(in, path);
}

Elaboration elaborate(const Design& d, const Library* lib, const std::string& filename) {
  return Elaborator(d, lib, filename).run();
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
void write_verilog_design(std::ostream& out, const Design& d) {
  // A net reference: bus bits keep their bus syntax, other names are escaped if needed.
  auto ref = [&](NetId n) -> std::string {
    if (n == kConst0Net) return "1'b0";
    if (n == kConst1Net) return "1'b1";
    const std::string& name = d.net_name(n);
    const auto br = name.rfind('[');
    if (br != std::string::npos && name.back() == ']' && d.buses.count(name.substr(0, br)))
      return verilog_identifier(name.substr(0, br)) + name.substr(br);
    return verilog_identifier(name);
  };
  auto range_text = [](const BitRange& r) { return "[" + std::to_string(r.msb) + ":" + std::to_string(r.lsb) + "] "; };

  out << "// written by rithereal\n";
  out << "module " << verilog_identifier(d.name) << " (";
  for (std::size_t i = 0; i < d.ports.size(); ++i)
    out << (i ? ", " : " ") << verilog_identifier(d.ports[i].name);
  out << " );\n";

  std::set<std::string> port_names, declared_buses;
  for (const auto& p : d.ports) {
    out << (p.dir == PortDir::Input ? "  input  " : "  output ") << (p.range ? range_text(*p.range) : "")
        << verilog_identifier(p.name) << ";\n";
    port_names.insert(p.name);
    for (const auto& b : d.port_bits(p)) port_names.insert(b);
  }
  bool any_wire = false;
  for (NetId n = 0; n < d.net_count(); ++n) {
    const std::string& name = d.net_name(n);
    if (port_names.count(name)) continue;
    const auto br = name.rfind('[');
    if (br != std::string::npos && name.back() == ']') {
      const std::string base = name.substr(0, br);
      auto it = d.buses.find(base);
      if (it != d.buses.end()) {
        if (port_names.count(base) || !declared_buses.insert(base).second) continue;
        out << "  wire   " << range_text(it->second) << verilog_identifier(base) << ";\n";
        any_wire = true;
        continue;
      }
    }
    out << "  wire   " << verilog_identifier(name) << ";\n";
    any_wire = true;
  }
  if (any_wire || !d.ports.empty()) out << "\n";

  std::set<std::string> helpers;
  for (const auto& [is_assign, i] : d.order()) {
    if (is_assign) {
      const Assign& a = d.assigns[i];
      out << "  assign " << ref(a.lhs) << " = " << (a.invert ? "~" : "") << ref(a.rhs) << ";\n";
      continue;
    }
    const Instance& inst = d.instances[i];
    if (kHelperModules.count(inst.type)) helpers.insert(inst.type);
    out << "  " << (inst.positional ? inst.type : verilog_identifier(inst.type));
    if (!inst.name.empty()) out << " " << verilog_identifier(inst.name);
    out << " (";
    for (std::size_t k = 0; k < inst.conns.size(); ++k) {
      const auto& c = inst.conns[k];
      out << (k ? ", " : " ");
      if (inst.positional) out << ref(c.net);
      else out << "." << c.pin << "(" << (c.net == kNoNet ? "" : ref(c.net)) << ")";
    }
    out << " );\n";
  }
  out << "endmodule\n";
  if (helpers.count("rith_dff"))
    out << "\nmodule rith_dff (input C, input D, output reg Q);\n  always @(posedge C) Q <= D;\nendmodule\n";
  if (helpers.count("rith_dffn"))
    out << "\nmodule rith_dffn (input C, input D, output reg Q);\n  always @(negedge C) Q <= D;\nendmodule\n";
  if (helpers.count("rith_mux2"))
    out << "\nmodule rith_mux2 (input S, input A, input B, output Y);\n  assign Y = S ? B : A;\nendmodule\n";
}

void write_verilog_design_file(const std::string& path, const Design& d) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write '" + path + "'");
  write_verilog_design(out, d);
}

}  // namespace rith
