"""Rebuild extended-fonts.fontpack from the source fonts named in
fonts/Extended-Fonts/manifest.json. The result is byte-identical to the pack that
manifest describes (267,832,481 bytes, sha256 7cad7820…489de6); it is published as a
release asset of this repository and downloaded by the firmware (设置 → 阅读与字库).

Character set: every Han code point of Unicode 17 (unified and compatibility ideographs)
plus the non-Han characters of MiSans, without control and format characters.
Per code point the first font that has it wins: MiSans (Light; Semibold for 30:4),
then MiSans L3, then Plangothic P1, then Plangothic P2.
Rendering and file layout are build_fontpack.py's.

Sources (check them against the manifest's sha256 before use):
  MiSans L3       https://hyperos.mi.com/font-download/MiSans_L3.zip  ("MiSans L3.ttf")
  Plangothic      https://github.com/Losketch/Plangothic-Project/releases/tag/V2.9.5795
                  (PlangothicP1-Regular.ttf, PlangothicP2-Regular.ttf)
  UnicodeData.txt https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt
MiSans Light and Semibold are in tools/ttf-fonts/MiSans. Needs freetype-py and fonttools.

    python build_extended.py <repo> <MiSans-L3.ttf> <plangothic dir> <UnicodeData.txt> <out> [workers]

About 20 seconds on a 16-core machine.
"""
import hashlib, json, multiprocessing, sys, time, unicodedata
from pathlib import Path

repo, l3_path, plan_dir, ucd, out = sys.argv[1:6]
workers = int(sys.argv[6]) if len(sys.argv) > 6 else 8
sys.path.insert(0, f'{repo}/tools/fontpack')
import build_fontpack as bf  # noqa: E402
from fontTools.ttLib import TTFont  # noqa: E402

VARIANTS = [(20, 2), (25, 2), (30, 2), (30, 4), (35, 2), (40, 2), (45, 2), (50, 2)]
MISANS = f'{repo}/tools/ttf-fonts/MiSans'
def fonts_for(bpp):
    return [f'{MISANS}/MiSans-Semibold.ttf' if bpp == 4 else f'{MISANS}/MiSans-Light.ttf', l3_path,
            f'{plan_dir}/PlangothicP1-Regular.ttf', f'{plan_dir}/PlangothicP2-Regular.ttf']

def charset():
    han, first = set(), None
    for line in open(ucd, encoding='utf-8'):
        f = line.split(';'); cp, name = int(f[0], 16), f[1]
        if name.startswith('<CJK Ideograph') and name.endswith('First>'): first = cp
        elif name.startswith('<CJK Ideograph') and name.endswith('Last>'): han.update(range(first, cp + 1))
        elif name.startswith('CJK COMPATIBILITY IDEOGRAPH'): han.add(cp)
    light = set(TTFont(f'{MISANS}/MiSans-Light.ttf', lazy=True).getBestCmap())
    extra = {cp for cp in light - han if unicodedata.category(chr(cp)) not in ('Cc', 'Cf')}
    return han, han | extra

def assign(cps, bpp):
    """code point -> index of the font that supplies it."""
    remaining, owner = set(cps), {}
    for i, path in enumerate(fonts_for(bpp)):
        have = set(TTFont(path, lazy=True).getBestCmap()) & remaining
        for cp in have: owner[cp] = i
        remaining -= have
    return owner, remaining

def render(job):
    size, bpp, font_index, cps = job
    import freetype
    face = freetype.Face(fonts_for(bpp)[font_index])
    face.set_pixel_sizes(0, size)
    out = []
    for cp in cps:
        g = bf.render_glyph(face, cp, size, bpp)
        if g is not None:
            out.append((g.codepoint, g.size, g.bpp, g.width, g.height, g.x_offset, g.y_offset, g.advance, g.bitmap))
    return out

if __name__ == '__main__':
    t0 = time.time()
    han, cps = charset()
    print(f'charset: {len(cps)} code points ({len(han)} Han)', flush=True)
    jobs, counts = [], {}
    for size, bpp in VARIANTS:
        owner, missing = assign(cps, bpp)
        if missing: raise SystemExit(f'{size}:{bpp}: {len(missing)} code points have no source font')
        for i, path in enumerate(fonts_for(bpp)):
            mine = sorted(cp for cp, o in owner.items() if o == i)
            counts.setdefault(f'{size}:{bpp}', {})[Path(path).name.replace('MiSans L3', 'MiSans-L3')] = len(mine)
            for k in range(0, len(mine), 4000):
                jobs.append((size, bpp, i, mine[k:k + 4000]))
    print('per-variant sources:', json.dumps(counts['20:2']), flush=True)
    items = []
    with multiprocessing.Pool(workers) as pool:
        for n, chunk in enumerate(pool.imap_unordered(render, jobs), 1):
            items.extend(bf.GlyphItem(*g) for g in chunk)
            if n % 20 == 0 or n == len(jobs):
                print(f'  rendered {n}/{len(jobs)} jobs, {len(items)} glyphs, {time.time() - t0:.0f}s', flush=True)
    print('max bitmap bytes:', max(len(g.bitmap) for g in items), flush=True)
    bf.write_fontpack(items, Path(out))
    digest = hashlib.sha256(Path(out).read_bytes()).hexdigest()
    print('bytes', Path(out).stat().st_size, 'sha256', digest, f'{time.time() - t0:.0f}s')
    Path(out + '.counts.json').write_text(json.dumps(counts, indent=1))
