# SPDX-License-Identifier: GPL-2.0-only
# Word-loss check: spacing of a once-per-frame word (18-word frames).
import sys, numpy as np
for fn in sys.argv[1:]:
    w=np.fromfile(fn,dtype='>u4'); vals,cn=np.unique(w,return_counts=True)
    nf=len(w)/18; cand=[v for v,n in zip(vals,cn) if 0.8*nf<n<1.2*nf and v!=0xffffffff]
    v=cand[0]; pos=np.nonzero(w==v)[0]; d=np.diff(pos)
    print(fn.split('/')[-1],'frames %d marker %08x  spacing!=18: %d (%.2f%%)'%(len(pos),v,int((d!=18).sum()),100*(d!=18).mean()))
