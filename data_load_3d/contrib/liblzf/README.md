# Vendored liblzf 3.6

Upstream: http://dist.schmorp.de/liblzf/liblzf-3.6.tar.gz
Version:  3.6 (2011)
SHA256:   9c5de01f7b9ccae40c3f619d26a7abec9986c06c36d260c179cedd04b89fb46a (liblzf-3.6.tar.gz)
License:  BSD-2-Clause (see LICENSE)
Author:   Marc Alexander Lehmann <schmorp@schmorp.de>

Only lzf.h, lzfP.h, lzf_c.c, lzf_d.c are vendored (the LZF codec). Sources are
unmodified. Used by pcd_reader.cpp to decode PCD `binary_compressed` bodies
(lzf_decompress) and by the PCD unit test to synthesize fixtures (lzf_compress).
