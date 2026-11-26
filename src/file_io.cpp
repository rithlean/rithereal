// SPDX-License-Identifier: Apache-2.0
#include <fstream>
#include <string>

#include "rith/io.hpp"

namespace rith {

namespace {
bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
enum class Format { Bench, Verilog };
Format format_of(const std::string& path) {
  if (ends_with(path, ".bench")) return Format::Bench;
  if (ends_with(path, ".v") || ends_with(path, ".sv") || ends_with(path, ".vg")) return Format::Verilog;
  throw std::runtime_error("cannot tell the format of '" + path + "' (use .bench or .v)");
}
}  // namespace

Netlist read_netlist_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open '" + path + "'");
  return format_of(path) == Format::Bench ? read_bench(in, path) : read_verilog(in, path);
}

void write_netlist_file(const std::string& path, const Netlist& nl) {
  const Format f = format_of(path);
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write '" + path + "'");
  if (f == Format::Bench) write_bench(out, nl);
  else write_verilog(out, nl);
}

}  // namespace rith
