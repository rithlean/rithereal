// SPDX-License-Identifier: Apache-2.0
// Scan insertion outputs: text report, DEF SCANCHAINS, STIL protocol, Tessent setup.
#include <algorithm>
#include <cstdio>
#include <map>
#include <ostream>
#include <set>
#include <string>

#include "rith/scan.hpp"

namespace rith {

namespace {

std::vector<std::string> clocks_of(const ScanResult& r) {
  std::vector<std::string> v;
  for (const auto& c : r.chains)
    if (std::find(v.begin(), v.end(), c.clock) == v.end()) v.push_back(c.clock);
  return v;
}

std::size_t longest(const ScanResult& r) {
  std::size_t n = 0;
  for (const auto& c : r.chains) n = std::max(n, c.cells.size());
  return n;
}

std::vector<std::string> bits(const Design& d, PortDir dir) {
  std::vector<std::string> v;
  for (const auto& p : d.ports)
    if (p.dir == dir)
      for (const auto& b : d.port_bits(p)) v.push_back(b);
  return v;
}

// DEF names: escape the hierarchy divider and bus brackets.
std::string def_name(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '/' || c == '[' || c == ']') o += '\\';
    o += c;
  }
  return o;
}

std::string q(const std::string& s) { return "\"" + s + "\""; }

std::string join_q(const std::vector<std::string>& v) {
  std::string o;
  for (std::size_t i = 0; i < v.size(); ++i) o += (i ? " + " : "") + q(v[i]);
  return o;
}

std::string repeat(std::size_t n, const char* what) {
  if (n == 1) return what;
  return "\\r" + std::to_string(n) + " " + what;
}

}  // namespace

void write_scan_report(std::ostream& out, const Design& d, const ScanResult& r) {
  std::size_t shortest = r.chains.empty() ? 0 : r.chains[0].cells.size();
  for (const auto& c : r.chains) shortest = std::min(shortest, c.cells.size());
  out << "scan insertion report for " << d.name << "\n\n";
  out << "scan enable   " << r.se_port << " (active high)\n";
  out << "chains        " << r.chains.size() << " (longest " << longest(r) << ", shortest " << shortest << ")\n";
  out << "flops         " << r.scanned() << " on chains (" << r.replaced << " replaced, " << r.restitched
      << " were already scan cells), " << r.excluded.size() << " left off\n\n";
  out << "chain      length  clock                 scan in         scan out\n";
  for (const auto& c : r.chains) {
    char line[256];
    std::snprintf(line, sizeof line, "%-10s %6zu  %-21s %-15s %s%s\n", c.name.c_str(), c.cells.size(),
                  (c.clock + (c.falling_edge ? " falling" : " rising")).c_str(), c.si_port.c_str(), c.so_port.c_str(),
                  c.inverted() ? " (inverted)" : "");
    out << line;
  }
  if (!r.replaced_cells.empty()) {
    out << "\ncell replacements\n";
    for (const auto& [from, to] : r.replaced_cells) out << "  " << from << " -> " << to << "\n";
  }
  if (!r.excluded.empty()) {
    out << "\nflops left off the chains\n";
    for (const auto& [inst, why] : r.excluded) out << "  " << inst << ": " << why << "\n";
  }
  for (const auto& w : r.warnings) out << "\nwarning: " << w << "\n";
  out << "\nchain order (scan-in first)\n";
  for (const auto& c : r.chains) {
    out << "  " << c.name << ":";
    for (const auto& cell : c.cells) out << " " << cell.name << (cell.inverting ? "(" + cell.out_pin + ")" : "");
    out << "\n";
  }
}

void write_def_scanchains(std::ostream& out, const Design& d, const ScanResult& r) {
  out << "VERSION 5.8 ;\nDIVIDERCHAR \"/\" ;\nBUSBITCHARS \"[]\" ;\n";
  out << "DESIGN " << def_name(d.name) << " ;\n\n";
  out << "SCANCHAINS " << r.chains.size() << " ;\n";
  for (const auto& c : r.chains) {
    out << "  - " << c.name << "\n";
    out << "    + PARTITION " << c.clock << (c.falling_edge ? "_falling" : "_rising") << "\n";
    out << "    + START PIN " << def_name(c.si_port) << "\n";
    out << "    + FLOATING\n";
    for (const auto& cell : c.cells)
      out << "      " << def_name(cell.name) << " ( IN " << cell.si_pin << " ) ( OUT " << cell.out_pin << " )\n";
    out << "    + STOP PIN " << def_name(c.so_port) << " ;\n";
  }
  out << "END SCANCHAINS\n\nEND DESIGN\n";
}

void write_stil_protocol(std::ostream& out, const Design& d, const ScanResult& r) {
  const auto clocks = clocks_of(r);
  const std::set<std::string> clock_set(clocks.begin(), clocks.end());
  std::set<std::string> si, so;
  for (const auto& c : r.chains) {
    si.insert(c.si_port);
    so.insert(c.so_port);
  }
  const auto ins = bits(d, PortDir::Input), outs = bits(d, PortDir::Output);
  std::vector<std::string> data_in, si_v, so_v;
  for (const auto& b : ins)
    if (!clock_set.count(b)) data_in.push_back(b);
  for (const auto& c : r.chains) {
    si_v.push_back(c.si_port);
    so_v.push_back(c.so_port);
  }

  out << "STIL 1.0 { Design 2005; }\n";
  out << "Header {\n  Title \"" << d.name << " scan protocol\";\n  Source \"written by rithereal\";\n}\n\n";
  out << "Signals {\n";
  for (const auto& b : ins) out << "  " << q(b) << " In" << (si.count(b) ? " { ScanIn; }" : ";") << "\n";
  for (const auto& b : outs) out << "  " << q(b) << " Out" << (so.count(b) ? " { ScanOut; }" : ";") << "\n";
  out << "}\n\n";
  out << "SignalGroups {\n";
  out << "  \"_pi\" = '" << join_q(ins) << "';\n";
  out << "  \"_in\" = '" << join_q(data_in) << "';\n";
  out << "  \"_po\" = '" << join_q(outs) << "';\n";
  out << "  \"_clk\" = '" << join_q(clocks) << "';\n";
  out << "  \"_si\" = '" << join_q(si_v) << "' { ScanIn; }\n";
  out << "  \"_so\" = '" << join_q(so_v) << "' { ScanOut; }\n";
  out << "}\n\n";
  out << "ScanStructures {\n";
  for (const auto& c : r.chains) {
    out << "  ScanChain " << q(c.name) << " {\n";
    out << "    ScanLength " << c.cells.size() << ";\n";
    out << "    ScanIn " << q(c.si_port) << ";\n";
    out << "    ScanOut " << q(c.so_port) << ";\n";
    out << "    ScanInversion " << (c.inverted() ? 1 : 0) << ";\n";
    out << "    ScanCells";
    bool inv = false;
    for (const auto& cell : c.cells) {
      out << "\n      " << (inv ? "! " : "") << q(cell.name + "." + cell.si_pin);
      inv = cell.inverting;
    }
    out << ";\n";
    out << "    ScanMasterClock " << q(c.clock) << ";\n";
    out << "  }\n";
  }
  out << "}\n\n";
  out << "Timing {\n  WaveformTable \"_default_WFT_\" {\n    Period '100ns';\n    Waveforms {\n";
  out << "      \"_in\" { 01ZN { '0ns' D/U/Z/N; } }\n";
  out << "      \"_clk\" { 01ZN { '0ns' D/U/Z/N; } }\n";
  out << "      \"_clk\" { P { '0ns' D; '45ns' U; '55ns' D; } }\n";
  out << "      \"_po\" { LHTX { '0ns' X; '40ns' L/H/T/X; } }\n";
  out << "    }\n  }\n}\n\n";
  out << "PatternBurst \"_burst_\" { PatList { \"_pattern_\" { } } }\n";
  out << "PatternExec { PatternBurst \"_burst_\"; }\n\n";

  const std::size_t nc = clocks.size(), nsi = si_v.size(), nso = so_v.size();
  out << "Procedures {\n";
  out << "  \"capture\" {\n    W \"_default_WFT_\";\n";
  out << "    F { " << q(r.se_port) << " = 0; }\n";
  out << "    V { \"_pi\" = \\r" << ins.size() << " #; \"_po\" = \\r" << outs.size() << " #; }\n  }\n";
  for (const auto& clk : clocks) {
    out << "  \"capture_" << clk << "\" {\n    W \"_default_WFT_\";\n";
    out << "    F { " << q(r.se_port) << " = 0; }\n";
    out << "    V { \"_pi\" = \\r" << ins.size() << " #; \"_po\" = \\r" << outs.size() << " #; }\n";
    out << "    V { " << q(clk) << " = P; }\n  }\n";
  }
  out << "  \"load_unload\" {\n    W \"_default_WFT_\";\n";
  out << "    C { \"_clk\" = " << repeat(nc, "0") << "; " << q(r.se_port) << " = 1; \"_so\" = " << repeat(nso, "X")
      << "; }\n";
  out << "    V { " << q(r.se_port) << " = 1; }\n";
  out << "    Shift { V { \"_si\" = " << repeat(nsi, "#") << "; \"_so\" = " << repeat(nso, "#") << "; \"_clk\" = "
      << repeat(nc, "P") << "; } }\n  }\n";
  out << "}\n\n";
  out << "MacroDefs {\n  \"test_setup\" {\n    W \"_default_WFT_\";\n";
  out << "    V { \"_clk\" = " << repeat(nc, "0") << "; " << q(r.se_port) << " = 0; }\n  }\n}\n";
}

void write_tessent_dofile(std::ostream& out, const Design& d, const ScanResult& r, const std::string& testproc_path) {
  out << "// Tessent setup for " << d.name << ", written by rithereal\n";
  for (const auto& clk : clocks_of(r)) out << "add_clocks 0 " << clk << "\n";
  out << "add_input_constraints " << r.se_port << " -c0\n";
  out << "add_scan_groups grp1 " << testproc_path << "\n";
  for (const auto& c : r.chains) out << "add_scan_chains " << c.name << " grp1 " << c.si_port << " " << c.so_port << "\n";
}

void write_tessent_testproc(std::ostream& out, const Design& d, const ScanResult& r) {
  const auto clocks = clocks_of(r);
  out << "// Tessent test procedures for " << d.name << ", written by rithereal\n";
  out << "set_time_scale 1.000000 ns ;\n\n";
  out << "timeplate gen_tp1 =\n   force_pi 0 ;\n   measure_po 40 ;\n";
  for (const auto& clk : clocks) out << "   pulse " << clk << " 45 10 ;\n";
  out << "   period 100 ;\nend;\n\n";
  out << "procedure test_setup =\n   timeplate gen_tp1 ;\n   cycle =\n      force " << r.se_port << " 0 ;\n";
  for (const auto& clk : clocks) out << "      force " << clk << " 0 ;\n";
  out << "   end ;\nend;\n\n";
  out << "procedure shift =\n   scan_group grp1 ;\n   timeplate gen_tp1 ;\n   cycle =\n      force_sci ;\n"
         "      measure_sco ;\n";
  for (const auto& clk : clocks) out << "      pulse " << clk << " ;\n";
  out << "   end;\nend;\n\n";
  out << "procedure load_unload =\n   scan_group grp1 ;\n   timeplate gen_tp1 ;\n   cycle =\n";
  for (const auto& clk : clocks) out << "      force " << clk << " 0 ;\n";
  out << "      force " << r.se_port << " 1 ;\n   end ;\n   apply shift " << longest(r) << " ;\nend;\n";
}

}  // namespace rith
