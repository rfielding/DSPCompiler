( do
 
  (in w0 w1 w2 w3 a x y0 y1)
  
  (vset output  (vadd (vmul w0 w1) (vsmul w2 w3)))
  (vset output1 (vadd a (vmul x (vadd y0 y1))))
  (vset output2 (vadd c (vmul output1 (vsadd y2 sy3))))
  
  (out output2)
)