// Design Compiler style mapped netlist using tiny.lib
module counter_dc ( clk, rst_n, en, a, b, sel, si, se, \cnt[1] , \cnt[0] , y, co, qn_out );
  input clk, rst_n, en, a, b, sel, si, se;
  output \cnt[1] , \cnt[0] , y, co, qn_out;
  wire   n1, n2, n3, n4, n5, n6, tie1, SYNOPSYS_UNCONNECTED_1;

  DFF_X1 \cnt_reg[0]  ( .D(n1), .CK(clk), .Q(\cnt[0] ), .QN(n2) );
  DFFR_X1 \cnt_reg[1]  ( .D(n3), .RN(rst_n), .CK(clk), .Q(\cnt[1] ) );
  SDFF_X1 scan_reg ( .D(n6), .SE(se), .SI(si), .CK(clk), .Q(qn_out) );
  XOR2_X1 U1 ( .A(\cnt[0] ), .B(en), .Z(n1) );
  NAND2_X1 U2 ( .A1(\cnt[0] ), .A2(en), .ZN(n4) );
  INV_X1 U3 ( .A(n4), .ZN(n5) );
  XOR2_X1 U4 ( .A(\cnt[1] ), .B(n5), .Z(n3) );
  AOI21_X1 U5 ( .A(a), .B1(b), .B2(n2), .ZN(n6) );
  MUX2_X1 U6 ( .A(n6), .B(tie1), .S(sel), .Z(y) );
  TIEHI_X1 U7 ( .Z(tie1) );
  HA_X1 U8 ( .A(a), .B(b), .CO(co), .S(SYNOPSYS_UNCONNECTED_1) );
  FILLCELL_X1 FILLER_1 ( );
endmodule
