( do
    (in x y z one b c)
    (vset w (vadd (vmul x y)(vsmul z one)))
    (vset a (vadd (vmul b c)(vmul z w)))
    (out w)
)