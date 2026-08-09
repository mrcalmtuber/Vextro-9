#!/usr/bin/env python3
"""Read a ZIM archive from the host.

Written from the format description rather than from src/zim.h, so the
two are independent and agreeing on real data means something. The
kernel reads ZIM to answer questions; this reads it to build the training
set for the model that phrases those answers, and the same code checks
that both see the same articles.

Stdlib only. Python 3.14 carries zstd, which is the compression every
modern ZIM cluster uses; zlib, bz2 and lzma are older and also handled.

    python3 tools/zimread.py assets/wiki.zim Moon
    python3 tools/zimread.py assets/wiki.zim --count
"""
import struct
import sys

try:
    from compression import zstd as _zstd          # Python 3.14+
except ImportError:                                # pragma: no cover
    _zstd = None
import bz2
import lzma
import zlib

ZIM_MAGIC = 0x044D495A
REDIRECT = 0xFFFF


class Zim:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(80)
        magic, = struct.unpack('<I', h[0:4])
        if magic != ZIM_MAGIC:
            raise ValueError('not a ZIM file')
        (self.article_count, self.cluster_count) = struct.unpack('<II', h[24:32])
        (self.url_ptr, self.title_ptr, self.cluster_ptr,
         self.mime_pos) = struct.unpack('<QQQQ', h[32:64])
        self.main_page, = struct.unpack('<I', h[64:68])
        self._cluster_cache = (None, None)

    # ---- directory ----

    def _read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def dirent_offset(self, index):
        return struct.unpack('<Q', self._read(self.url_ptr + index * 8, 8))[0]

    def dirent(self, index):
        off = self.dirent_offset(index)
        b = self._read(off, 512)
        mime, param_len, ns = struct.unpack('<HBc', b[0:4])
        ns = ns.decode('latin1')
        if mime == REDIRECT:
            redirect, = struct.unpack('<I', b[8:12])
            cluster = blob = None
            p = 12
        else:
            cluster, blob = struct.unpack('<II', b[8:16])
            redirect = None
            p = 16
        end = b.index(b'\0', p)
        url = b[p:end].decode('utf-8', 'replace')
        p = end + 1
        end = b.index(b'\0', p)
        title = b[p:end].decode('utf-8', 'replace') or url
        return {'index': index, 'mime': mime, 'ns': ns, 'url': url,
                'title': title, 'cluster': cluster, 'blob': blob,
                'redirect': redirect}

    def resolve(self, index, hops=8):
        for _ in range(hops):
            e = self.dirent(index)
            if e['redirect'] is None:
                return e
            index = e['redirect']
        return None

    # ---- content ----

    def _cluster(self, num):
        if self._cluster_cache[0] == num:
            return self._cluster_cache[1]
        start, = struct.unpack('<Q', self._read(self.cluster_ptr + num * 8, 8))
        end, = struct.unpack('<Q', self._read(self.cluster_ptr + (num + 1) * 8, 8)) \
            if num + 1 < self.cluster_count else (self._file_size(),)
        raw = self._read(start, end - start)
        info = raw[0]
        comp, extended = info & 0x0F, bool(info & 0x10)
        body = raw[1:]
        if comp in (0, 1):
            data = body
        elif comp == 2:
            data = zlib.decompress(body)
        elif comp == 3:
            data = bz2.decompress(body)
        elif comp == 4:
            data = lzma.decompress(body)
        elif comp == 5:
            if _zstd is None:
                raise RuntimeError('this Python has no zstd')
            data = _zstd.decompress(body)
        else:
            raise ValueError('cluster compression %d' % comp)

        width = 8 if extended else 4
        fmt = '<Q' if extended else '<I'
        first, = struct.unpack(fmt, data[0:width])
        n = first // width
        offs = [struct.unpack(fmt, data[i * width:(i + 1) * width])[0]
                for i in range(n)]
        self._cluster_cache = (num, (data, offs))
        return data, offs

    def _file_size(self):
        cur = self.f.tell()
        self.f.seek(0, 2)
        n = self.f.tell()
        self.f.seek(cur)
        return n

    def content(self, index):
        e = self.resolve(index)
        if e is None or e['cluster'] is None:
            return None, None
        data, offs = self._cluster(e['cluster'])
        b = e['blob']
        if b + 1 >= len(offs):
            return None, None
        return e, data[offs[b]:offs[b + 1]]

    # ---- lookup ----

    def lower_bound(self, ns, url):
        lo, hi = 0, self.article_count
        key = (ns, url)
        while lo < hi:
            mid = (lo + hi) // 2
            e = self.dirent(mid)
            if (e['ns'], e['url']) < key:
                lo = mid + 1
            else:
                hi = mid
        return lo

    def find(self, url, ns='C'):
        i = self.lower_bound(ns, url)
        if i >= self.article_count:
            return None
        e = self.dirent(i)
        return i if (e['ns'] == ns and e['url'] == url) else None


# ---- HTML to text, matching what the kernel does ----

def html_text(b, limit=20000):
    """Plain text, with style and script dropped whole.

    Mirrors wiki_html_text in src/apps.h -- including skipping those two
    elements entirely, which is the bug that once fed a stylesheet to the
    model as an article.
    """
    s = b.decode('utf-8', 'replace')
    out = []
    i, n = 0, min(len(s), limit)
    while i < n:
        c = s[i]
        if c == '<':
            if s.startswith('<!--', i):
                j = s.find('-->', i)
                i = n if j < 0 else j + 3
                continue
            low = s[i:i + 8].lower()
            for elem in ('style', 'script'):
                if low.startswith('<' + elem) and \
                   (len(s) > i + 1 + len(elem)) and \
                   (s[i + 1 + len(elem)] in '> \t\n\r/'):
                    j = s.lower().find('</' + elem, i)
                    j = n if j < 0 else s.find('>', j)
                    i = n if j < 0 else j + 1
                    break
            else:
                j = s.find('>', i)
                i = n if j < 0 else j + 1
                continue
            continue
        if c == '&':
            j = i + 1
            while j < n and s[j] not in '; ':
                j += 1
            i = j + 1 if j < n and s[j] == ';' else j
            out.append(' ')
            continue
        out.append(' ' if c in '\n\r\t' else c)
        i += 1
    text = ''.join(out)
    return ' '.join(text.split())


def main():
    z = Zim(sys.argv[1])
    if len(sys.argv) < 3 or sys.argv[2] == '--count':
        print('%d entries, %d clusters, main page %d'
              % (z.article_count, z.cluster_count, z.main_page))
        return 0
    idx = z.find(sys.argv[2])
    if idx is None:
        print('not found:', sys.argv[2])
        return 1
    e, blob = z.content(idx)
    print('%s  [%d bytes]' % (e['title'], len(blob)))
    print(html_text(blob)[:700])
    return 0


if __name__ == '__main__':
    sys.exit(main())
