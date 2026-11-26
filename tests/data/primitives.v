// Verilog gate primitives, ANSI header, implicit wires
module prim (input a, input b, input c, output y, output z);
  wire n1;
  nand g1 (n1, a, b);
  xor (n2, n1, c);
  not g3 (y, n2);
  or g4 (z, a, 1'b0, c);
endmodule
