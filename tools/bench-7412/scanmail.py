# SPDX-License-Identifier: GPL-2.0-only
# Scan a capture of FB-byte frames for AA-framed, LE16-sum-valid mails at
# every bit offset 0..15, slot and in-slot byte order.
import sys, re, numpy as np
fn, logf, FB = sys.argv[1], sys.argv[2], int(sys.argv[3])
raw=np.fromfile(fn,dtype=np.uint8); nfr=len(raw)//FB
probes=[(int(m.group(2)),m.group(0)[:70]) for m in re.finditer(r'probe seq (\d+) at rx frame (\d+).*',open(logf).read())]
bits=np.unpackbits(raw[:nfr*FB]); FBITS=FB*8
hits=[]
for off in range(16):
  idx=np.arange(nfr-1)[:,None]*FBITS+off+np.arange(FBITS)[None,:]
  f2=np.packbits(bits[idx],axis=1)
  for slot in range(FB//2):
    for sw in (0,1):
      b=f2[:,2*slot:2*slot+2]
      if sw: b=b[:,::-1]
      st=b.reshape(-1).astype(np.int64)
      for p in np.nonzero(st==0xAA)[0]:
          if p+8>len(st): continue
          L=int(st[p+3])<<8|int(st[p+2])
          if not 2<=L<=0xFB or p+L+8>len(st): continue
          s=int(st[p+2:p+L+6].sum())&0xffff
          cs=int(st[p+L+7])<<8|int(st[p+L+6])
          if s==cs: hits.append((int(p//2),off,slot,sw,L,bytes(st[p+4:p+8].astype(np.uint8)).hex()))
print('valid mails',len(hits))
for h in sorted(hits)[:25]:
    pr=[x for x in probes if x[0]<=h[0]]
    print(h,'| after',pr[-1][1] if pr else '-')
