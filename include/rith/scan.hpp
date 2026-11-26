// SPDX-License-Identifier: Apache-2.0
// Mux-D scan insertion on a mapped (library-cell) netlist.
//
//   1. Each flop cell is matched to a scan cell from the same library by
//      comparing Boolean functions, not names: in mission mode (scan enable
//      off) the scan cell must behave exactly like the original flop.
//   2. Flops are grouped by clock domain (the input port that clocks them and
//      the active edge), so chains need no lockup latches.
//   3. Each domain is cut into balanced chains, which are stitched through
//      the flops' Q outputs to new scan-in, scan-out and scan-enable ports.
//
// The result can be checked by simulation (verify_mission_mode, verify_shift)
// and written as DEF SCANCHAINS, a STIL protocol or a Tessent setup.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "rith/design.hpp"
#include "rith/liberty.hpp"

namespace rith {

// Scan pins of a mux-scan flop cell.
struct ScanPins {
  std::string si, se;
  bool se_active_high = true;
};

// The scan pins of a cell, if it is a mux-scan flop. Pins are identified by
// their Liberty signal_type, or failing that by common names (SI, SE, SCD,
// SCE, TI, TE), and are always confirmed from the next_state function.
std::optional<ScanPins> find_scan_pins(const LibCell& cell);

struct ScanEquivalent {
  const LibCell* cell = nullptr;  // the original cell when it is already a scan cell
  ScanPins pins;
};
// The scan cell that can replace a flop cell, preferring the closest name
// (DFFX1 -> SDFFX1). Only the outputs in `used_outputs` must exist on the scan
// cell (all outputs when null). Empty if the library has none.
std::optional<ScanEquivalent> find_scan_equivalent(const Library& lib, const LibCell& flop,
                                                   const std::vector<std::string>* used_outputs = nullptr);

struct ScanConfig {
  std::size_t chains = 0;      // total number of chains (at least one per clock domain)
  std::size_t max_length = 0;  // or: longest allowed chain; 0 = no limit
  std::string se_port = "test_se";    // reused if the design already has this input
  std::string si_prefix = "test_si";  // chains get test_si1, test_si2, ...
  std::string so_prefix = "test_so";
};

struct ScanCell {
  std::size_t instance = 0;  // index into Design::instances
  std::string name;          // instance name
  std::string si_pin;        // pin the chain enters by
  std::string out_pin;       // pin the chain leaves by (Q, or QN when there is no Q)
  bool inverting = false;    // out_pin is the inverted state
};

struct ScanChain {
  std::string name;  // chain1, chain2, ...
  std::string si_port, so_port;
  std::string clock;  // clock input port
  bool falling_edge = false;
  std::vector<ScanCell> cells;  // in shift order, scan-in first
  bool inverted() const;        // scan-out sees the scan-in data inverted
};

struct ScanResult {
  std::vector<ScanChain> chains;
  std::string se_port;
  std::vector<std::pair<std::string, std::string>> excluded;  // instance, reason
  std::map<std::string, std::string> replaced_cells;           // old cell -> scan cell
  std::size_t replaced = 0;   // flops swapped for a scan cell
  std::size_t restitched = 0; // flops that already were scan cells
  // Input ports that were the scan enable of restitched cells, with their
  // inactive value; mission-mode checks hold them there.
  std::map<std::string, bool> old_scan_enables;
  std::vector<std::string> warnings;
  std::size_t scanned() const;
};

// Inserts scan into `d` in place. Throws on configuration errors (for example
// fewer chains than clock domains, or a scan port name already in use).
ScanResult insert_scan(Design& d, const Library& lib, const ScanConfig& cfg = {});

struct ScanCheck {
  bool ok = true;
  std::string message;
  std::size_t patterns = 0;
};
// With scan enable off, the scanned design must match the original on every
// output and flop next state. Flops are matched by instance.
ScanCheck verify_mission_mode(const Elaboration& before, const Elaboration& after, const ScanResult& r,
                              std::size_t blocks = 64, std::uint64_t seed = 1);
// With scan enable on, random data shifted into each scan-in must reach every
// flop of its chain in order and come out of the scan-out after exactly the
// chain length, whatever the other inputs do.
ScanCheck verify_shift(const Elaboration& after, const ScanResult& r, std::uint64_t seed = 1);

// ---- outputs ----
void write_scan_report(std::ostream& out, const Design& d, const ScanResult& r);
// DEF SCANCHAINS section for placement-aware reordering (OpenROAD, Innovus).
void write_def_scanchains(std::ostream& out, const Design& d, const ScanResult& r);
// STIL 1.0 test protocol (ports, scan chains, shift and capture procedures).
void write_stil_protocol(std::ostream& out, const Design& d, const ScanResult& r);
// Tessent: a dofile that declares clocks and chains, and its test procedure file.
void write_tessent_dofile(std::ostream& out, const Design& d, const ScanResult& r, const std::string& testproc_path);
void write_tessent_testproc(std::ostream& out, const Design& d, const ScanResult& r);

}  // namespace rith
