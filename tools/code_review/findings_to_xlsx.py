"""コードレビューの指摘の文書(docs/plans/code-review-findings.md)を、1件1行のExcelの一覧にする。

使い方: python tools/code_review/findings_to_xlsx.py <findings.md> <out.xlsx> [生成元のコミット] [--force]
生成元のコミットを省くと、指摘の文書を最後に変えたコミットをgitから取る。
既にある一覧の「判断」「メモ」に書き込みがあると、--force が無い限り上書きしない。
検査として、項目の数、判断・確度の欠け、種類2の根拠の強さの欠け、区切りごとの数を標準出力に出す。
openpyxlが要る。
"""
import io
import os
import re
import subprocess
import sys
from collections import Counter, OrderedDict

from openpyxl import Workbook
from openpyxl.styles import Alignment, Font, PatternFill
from openpyxl.utils import get_column_letter

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8')

args = [a for a in sys.argv[1:] if a != '--force']
force = '--force' in sys.argv[1:]
if len(args) not in (2, 3):
    sys.exit(__doc__)
src, out = args[0], args[1]

# 一覧の「判断」「メモ」は利用者が書き込む欄で、作り直すと消える。
# 書き込みのある一覧は、--force を付けない限り上書きしない。
if os.path.exists(out) and not force:
    from openpyxl import load_workbook
    old = load_workbook(out, read_only=True)
    if '指摘' in old.sheetnames:
        ows = old['指摘']
        ohdr = [c.value for c in next(ows.iter_rows(min_row=1, max_row=1))]
        if '判断' in ohdr and 'メモ' in ohdr:
            ji, mi = ohdr.index('判断'), ohdr.index('メモ')
            edited = [r[0] for r in ows.iter_rows(min_row=2, values_only=True)
                      if (r[ji] not in (None, '未')) or (r[mi] not in (None, ''))]
            if edited:
                sys.exit(f'{out} の判断・メモに書き込みがある({len(edited)}件、例: {", ".join(map(str, edited[:5]))})。'
                         '指摘の文書へ書き戻してから作り直すか、--force を付ける')
    old.close()
if len(args) == 3:
    commit = args[2]
else:
    srcdir = os.path.dirname(os.path.abspath(src))
    commit = subprocess.run(['git', '-C', srcdir, 'log', '-1', '--format=%h', '--', os.path.abspath(src)],
                            capture_output=True, text=True, check=True).stdout.strip()
    dirty = subprocess.run(['git', '-C', srcdir, 'diff', '--quiet', 'HEAD', '--', os.path.abspath(src)]).returncode != 0
    if dirty:
        commit += '(未コミットの変更を含む)'
with open(src, encoding='utf-8') as f:
    lines = f.read().split('\n')

TYPE_NAMES = {'1': '不具合', '2': 'legacyとの違い', '3': '不一致', '4': '未使用・古い記述', '5': '決めた仕様との食い違い'}
REGION_RE = re.compile(r'^## 区切り(\d) (.+?)\((\d{4})年(\d{1,2})月(\d{1,2})日、HEAD `([0-9a-f]+)`\)')
SUBHEAD_RE = re.compile(r'^\*\*([^*]+)\*\*')
ITEM_RE = re.compile(r'^- \*\*([1-6]-\d{2})\*\* (.*)$')
HEAD_RE = re.compile(r'^([1-5](?:・[1-5])*)\s*(?:/\s*(.*?))?\s+—\s+(.*)$', re.S)
KONKYO_RE = re.compile(r'根拠(?:\([^)]*\))?: ')
STRENGTH_RE = re.compile(r'^(指示|強|弱|無)')

regions = OrderedDict()
rows = []
region = None
subhead = ''
i = 0
while i < len(lines):
    line = lines[i]
    m = REGION_RE.match(line)
    if m:
        region = m.group(1)
        regions[region] = {'name': m.group(2), 'head': m.group(6)}
        subhead = ''
        i += 1
        continue
    if region and line.startswith('**') and not line.startswith('- '):
        sm = SUBHEAD_RE.match(line)
        if sm:
            subhead = sm.group(1)
    m = ITEM_RE.match(line)
    if not (region and m):
        i += 1
        continue
    num, rest = m.group(1), m.group(2)
    # 字下げした続きの行(箇条書きと、その後の段落)を同じ項目に含める
    cont = []
    j = i + 1
    while j < len(lines):
        nxt = lines[j]
        if nxt.startswith('  '):
            cont.append(nxt.strip())
            j += 1
        elif nxt == '' and j + 1 < len(lines) and lines[j + 1].startswith('  '):
            j += 1
        else:
            break
    text = rest + ('\n' + '\n'.join(cont) if cont else '')
    i = j

    hm = HEAD_RE.match(text)
    if not hm:
        print('PARSE-FAIL', num, text[:60])
        continue
    types, loc, body = hm.group(1), (hm.group(2) or ''), hm.group(3)

    # 「判断: 未」の行の後に、測定の記録や後の区切りでの追記が字下げで続く項目がある。
    # 判断はその行の終わりまでとし、続きは内容の末尾へ回す。
    judge = ''
    tail = ''
    k = body.rfind('判断: ')
    if k >= 0:
        e = body.find('\n', k)
        judge = (body[k + len('判断: '):] if e < 0 else body[k + len('判断: '):e]).strip()
        tail = '' if e < 0 else body[e + 1:].strip()
        body = body[:k]
    certainty = ''
    k = body.rfind('確度: ')
    if k >= 0:
        certainty = body[k + len('確度: '):].strip().rstrip('。')
        body = body[:k]
    konkyo = ''
    km = None
    for km_ in KONKYO_RE.finditer(body):
        km = km_
    if km:
        konkyo = body[km.end():].strip().rstrip('。')
        body = body[:km.start()]
    # 強さは、「数では弱に入れた」の指定があればそれ、無ければ根拠の先頭の語。
    # 先頭に無いときは、文中の「指示(」等が1種類だけならそれを使う。
    strength = ''
    if '2' in types.split('・') and km and km.group(0) == '根拠: ':
        om = re.search(r'数では(指示|強|弱|無)に入れた', konkyo)
        sm = STRENGTH_RE.match(konkyo)
        found = set(re.findall(r'(指示|強|弱|無)\(', konkyo))
        if om:
            strength = om.group(1)
        elif sm:
            strength = sm.group(1)
        elif len(found) == 1:
            strength = found.pop()
    if tail:
        body = body.rstrip() + '\n' + tail

    def clean(s):
        return s.replace('`', '').replace('**', '').strip()

    rows.append({
        'num': num,
        'region': f'{region} {regions[region]["name"]}',
        'region_no': region,
        'subhead': subhead,
        'types': types,
        'type_names': '・'.join(TYPE_NAMES[t] for t in types.split('・')),
        'loc': clean(loc),
        'body': clean(body).rstrip(),
        'strength': strength,
        'konkyo': clean(konkyo),
        'certainty': clean(certainty),
        'judge': judge,
        'head': regions[region]['head'],
    })

# ---- 検査 ----
print('items', len(rows))
print('per region', dict(Counter(r['region_no'] for r in rows)))
print('missing judge', [r['num'] for r in rows if not r['judge']])
print('missing certainty', [r['num'] for r in rows if not r['certainty']])
print('type2 without strength', [r['num'] for r in rows if '2' in r['types'].split('・') and not r['strength']])
print('judge values', dict(Counter(r['judge'] for r in rows)))
dups = [n for n, c in Counter(r['num'] for r in rows).items() if c > 1]
print('duplicate numbers', dups)
for reg in regions:
    c = Counter(r['strength'] for r in rows if r['region_no'] == reg and r['strength'])
    print('strength', reg, {s: c.get(s, 0) for s in ('指示', '強', '弱', '無')})
print('bugs per region', dict(Counter(r['region_no'] for r in rows if '1' in r['types'].split('・'))))

# ---- 書き出し ----
FONT = 'Meiryo'
base = Font(name=FONT, size=10)
bold = Font(name=FONT, size=10, bold=True)
head_fill = PatternFill('solid', fgColor='D9D9D9')
input_fill = PatternFill('solid', fgColor='FFFF00')
wrap_top = Alignment(wrap_text=True, vertical='top')

wb = Workbook()
ws = wb.active
ws.title = '指摘'
cols = [
    ('番号', 'num', 7), ('区切り', 'region', 16), ('節', 'subhead', 14),
    ('種類', 'types', 6), ('種類(名前)', 'type_names', 16), ('場所', 'loc', 30),
    ('内容', 'body', 80), ('根拠の強さ', 'strength', 8), ('根拠', 'konkyo', 30),
    ('確度', 'certainty', 24), ('判断', 'judge', 14), ('メモ', None, 30),
    ('行番号の基準のHEAD', 'head', 10),
]
for c, (title, _, width) in enumerate(cols, 1):
    cell = ws.cell(row=1, column=c, value=title)
    cell.font = bold
    cell.fill = head_fill
    cell.alignment = wrap_top
    ws.column_dimensions[get_column_letter(c)].width = width
for r, row in enumerate(rows, 2):
    for c, (title, key, _) in enumerate(cols, 1):
        cell = ws.cell(row=r, column=c, value=row[key] if key else None)
        cell.font = base
        cell.alignment = wrap_top
        if title in ('判断', 'メモ'):
            cell.fill = input_fill
ws.freeze_panes = 'B2'
ws.auto_filter.ref = f'A1:{get_column_letter(len(cols))}{len(rows) + 1}'

# 区切りごとの数(判断の欄を埋めると「判断が未」が減る)
ss = wb.create_sheet('区切りごとの数')
hdr = ['区切り', '件数', '不具合を含む', '判断が未']
for c, t in enumerate(hdr, 1):
    cell = ss.cell(row=1, column=c, value=t)
    cell.font = bold
    cell.fill = head_fill
last = len(rows) + 1
B = f"'指摘'!$B$2:$B${last}"
D = f"'指摘'!$D$2:$D${last}"
K = f"'指摘'!$K$2:$K${last}"
for r, reg in enumerate(regions, 2):
    label = f'{reg} {regions[reg]["name"]}'
    ss.cell(row=r, column=1, value=label).font = base
    ss.cell(row=r, column=2, value=f'=COUNTIF({B},A{r})').font = base
    ss.cell(row=r, column=3, value=f'=COUNTIFS({B},A{r},{D},"*1*")').font = base
    ss.cell(row=r, column=4, value=f'=COUNTIFS({B},A{r},{K},"未")').font = base
tr = len(regions) + 2
ss.cell(row=tr, column=1, value='計').font = bold
for c in (2, 3, 4):
    col = get_column_letter(c)
    ss.cell(row=tr, column=c, value=f'=SUM({col}2:{col}{tr - 1})').font = bold
note = ss.cell(row=tr + 2, column=1,
               value='「不具合を含む」は、種類に1を含む項目(1・2等)を数える。legacyとの違いの強さ別の数は、指摘の文書の「区切りごとの数」の表にある(数え方の注記つき)。')
note.font = base
ss.column_dimensions['A'].width = 34
for col in 'BCD':
    ss.column_dimensions[col].width = 12

# 読み方
rd = wb.create_sheet('読み方')
legend = [
    ('生成元', f'docs/plans/code-review-findings.md(コミット {commit})。この一覧は写しで、元は指摘の文書'),
    ('作り方', '指摘の文書の番号つきの項目を、1件1行に機械的に分けた。文言は変えていない(コードの印のバッククォートだけ外した)'),
    ('書き込む欄', '黄色の「判断」と「メモ」。ほかの欄は指摘の文書の写し'),
    ('判断の欄', '今は全件「未」。書き方は決めていない。指摘の文書へ書き戻すときに形を決める'),
    ('行番号', '各区切りのHEAD(「行番号の基準のHEAD」の欄)のもの。コードを直すと行がずれるので、直す前に開いて確かめる'),
    ('種類', '1 不具合 / 2 legacyとの違い / 3 チップファミリー間の不一致(区切りによってはチャンネルの種類・バンクの種類・アプリの間の不一致を含む) / 4 使われていないコードと古いままの重複(古いコメント・文書の食い違いを含む) / 5 決めた仕様との食い違い。「1・2」は両方に当たる'),
    ('根拠の強さ', '種類2に付ける。指示 = 設計者が作り直しのときに変えるよう指示した / 強 = 資料・測定・テストで裏づけられる、または旧の不具合の修正 / 弱 = 文書・worklog・コミット・会話記録に理由があるが裏づけは無い / 無 = 見つからない'),
    ('会話記録の番号', 'X01-B055のような番号は、作り直しのときの会話記録の発言の番号(Bはユーザー、Kはアシスタント)'),
    ('確度', '確認済み = 確かめた(多くはコードを追って確かめた) / 推測 = 出典を示せない。空の項目は、文書の項目に確度が書かれていないもの(その区切りの冒頭の段落が扱いを書いている)'),
    ('内容の末尾の箇条書き', '項目の箇条書きと、「判断」の後に続いていた測定の記録や、後の区切りでの訂正・追記'),
    ('数式', '「区切りごとの数」の数はCOUNTIF・COUNTIFSの数式で、Excelで開いたときに計算される'),
]
for r, (k, v) in enumerate(legend, 1):
    a = rd.cell(row=r, column=1, value=k)
    a.font = bold
    a.alignment = wrap_top
    b = rd.cell(row=r, column=2, value=v)
    b.font = base
    b.alignment = wrap_top
rd.column_dimensions['A'].width = 16
rd.column_dimensions['B'].width = 110

wb.calculation.fullCalcOnLoad = True
wb.save(out)
print('saved', out)
