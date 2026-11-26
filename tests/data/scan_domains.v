// Scan insertion test design for tiny.lib: two clocks, both edges, an
// inverted-output flop, a gated clock and a flop with no scan version.
module domains ( clk1, clk2, gate_en, a, b, rn, \bus[1] , y1, y2, y3, q );
  input clk1, clk2, gate_en, a, b, rn, \bus[1] ;
  output y1, y2, y3;
  output [1:0] q;
  wire n1, q1, q2, qn3, q4, q5, q6, q7, q8, gclk, clk2_b;
  wire [3:0] spare;

  DFF_X1 r1 ( .D(a), .CK(clk1), .Q(q1) );
  DFFR_X1 \r2_reg[0]  ( .D(q1), .RN(rn), .CK(clk1), .Q(q2) );
  DFFQN_X1 r3 ( .D(n1), .CK(clk1), .QN(qn3) );
  NAND2_X1 u1 ( .A1(q2), .A2(b), .ZN(n1) );
  DFFN_X1 r4 ( .D(qn3), .CKN(clk1), .Q(q4) );
  INV_X1 u2 ( .A(clk2), .ZN(clk2_b) );
  DFF_X1 r5 ( .D(q4), .CK(clk2_b), .Q(q5) );
  DFF_X1 r6 ( .D(q5), .CK(clk2), .Q(q6) );
  AND3_X1 u3 ( .A1(clk1), .A2(gate_en), .A3(gate_en), .ZN(gclk) );
  DFF_X1 r7 ( .D(q6), .CK(gclk), .Q(q7) );
  DFFE_X1 r8 ( .D(a), .E(b), .CK(clk1), .Q(q8) );
  DFF_X1 r9 ( .D(\bus[1] ), .CK(clk1) );
  XOR2_X1 u4 ( .A(q7), .B(q6), .Z(y1) );
  assign y2 = q8;
  BUF_X1 u5 ( .A(q4), .Z(y3) );
  INV_X1 u6 ( .A(q1), .ZN(spare[2]) );
  assign q[1] = spare[2];
  assign q[0] = 1'b0;
endmodule
