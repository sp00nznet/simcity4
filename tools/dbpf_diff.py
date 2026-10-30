# Compare two SC4 saves (DBPF 1.0) against the file they started from:
# python tools/dbpf_diff.py PRISTINE.sc4 RECOMP.sc4 ORIGINAL.sc4
import struct, sys
def index(p):
    d = open(p, 'rb').read()
    assert d[:4] == b'DBPF'
    major, minor = struct.unpack_from('<II', d, 4)
    n, off, size = struct.unpack_from('<III', d, 36)
    ent = {}
    for i in range(n):
        t, g, inst, o, s = struct.unpack_from('<5I', d, off + 20 * i)
        ent[(t, g, inst)] = d[o:o + s]
    return (major, minor), ent
if __name__ == '__main__':
    pristine, a, b = sys.argv[1:4]
    vp, P = index(pristine); va, A = index(a); vb, B = index(b)
    print('version', vp, va, vb, 'entries', len(P), len(A), len(B))
    print('keys only in recomp', sorted(set(A) - set(B))[:10])
    print('keys only in original', sorted(set(B) - set(A))[:10])
    same = sum(A[k] == B[k] for k in set(A) & set(B))
    print('identical entries', same, 'of', len(set(A) & set(B)))
    # which types differ, and did the save change vs pristine for both
    from collections import Counter
    diff = Counter('%08X' % k[0] for k in set(A) & set(B) if A[k] != B[k])
    chP = Counter('%08X' % k[0] for k in set(A) & set(P) if A[k] != P[k])
    print('types differing recomp vs original:', dict(diff.most_common(12)))
    print('types the recomp changed vs pristine:', dict(chP.most_common(12)))
