# Checks for tests/todo_p0.sh: app stdout (debug traces), the todo data file and screenshots.
# Usage: python3 -I todo_p0_check.py <work-dir> <app-log>   exit 0 PASS, 1 FAIL
import os
import struct
import sys

from PIL import Image

work, log = sys.argv[1], sys.argv[2]
fails = []


def check(cond, msg):
    print(('ok   ' if cond else 'FAIL ') + msg)
    if not cond:
        fails.append(msg)


lines = open(log, encoding='latin1').read().split('\n')
bad = [l for l in lines if 'unimplemented function called' in l or 'unhandled exception' in l
       or 'Segmentation' in l or 'UNKNOWN CLASS' in l or 'fatal' in l]
check(not bad, 'no unimplemented calls, exceptions or unknown classes (%d hits)' % len(bad))
for l in bad[:5]:
    print('      ' + l[:160])


def section(name):
    """lines between '== name' and the next '== ' marker"""
    out, on = [], False
    for l in lines:
        if l.startswith('== '):
            on = (l == '== ' + name)
            continue
        if on:
            out.append(l)
    return out


def row_titles(ls):
    out = []
    for l in ls:
        if ('listview row ' in l or 'listview sorted row ' in l) and ' title="' in l:
            out.append(l.split(' title="', 1)[1].rsplit('"', 1)[0])
    return out


# data file: u32 count at 63200, 632-byte records from offset 4, title at +0, completed at +600
dat = os.path.join(work, 'home', '.config', 'TodoApp', 'todos.dat')
if check(os.path.exists(dat), 'data file written'):
    d = open(dat, 'rb').read()
    count = struct.unpack_from('<I', d, 63200)[0]
    recs = []
    for i in range(count):
        o = 4 + 632 * i
        t = d[o:o + 100].split(b'\0')[0].decode('latin1')
        done = struct.unpack_from('<I', d, o + 600)[0]
        recs.append((t, done))
    check(count == 2, 'data file holds 2 todos after delete (count=%d)' % count)
    check(sorted(r[0] for r in recs) == ['Alpha task', 'Buy oat milk'], 'data titles %r' % (recs,))
    check(any(t == 'Buy oat milk' and done == 1 for t, done in recs), 'completed flag stored for Buy oat milk')

check(sum(1 for l in section('add') if ' button ' in l and 'notify' in l) == 3,
      'three Add clicks each reach the handler')

# search: the last rebuild while "alpha" is typed lists only the matching todo
search = section('search')
starts = [i for i, l in enumerate(search) if 'listview row 0 ' in l]
last_group = row_titles(search[starts[-1]:]) if starts else []
check(last_group == ['Alpha task'], 'search filters to the matching todo (%r)' % (last_group,))

# P1: adding with an empty title shows the validation MessageBox
check(any('MessageBox caption="Error" text="Please enter a title!"' in l for l in section('empty_title')),
      'empty-title Add shows the validation MessageBox')

# LVM_SORTITEMSEX passes item indices to the comparator; header click must sort by title
sort_titles = row_titles(section('sort'))
check(sort_titles[:2] in (['Alpha task', 'Buy oat milk'], ['Buy oat milk', 'Alpha task']) and len(sort_titles) >= 2,
      'column sort produced a title order %r' % (sort_titles[:2],))
print('column sort order after header click: %r' % (sort_titles[:2],))

# restart: rows loaded from the file on the second start
restart = section('restart')
titles = row_titles(restart)
check(sorted(set(titles)) == ['Alpha task', 'Buy oat milk'] and len(titles) >= 2,
      'data survives restart; rows shown: %r' % (titles,))


def dark_pixels(path, box):
    if not os.path.exists(path):
        return -1
    im = Image.open(path).convert('L').crop(box)
    return sum(1 for v in im.get_flattened_data() if v < 110)


n = dark_pixels(os.path.join(work, 'after_restart.png'), (12, 60, 780, 200))
check(n > 300, 'list rows render text after restart (dark px=%d)' % n)
n = dark_pixels(os.path.join(work, 'after_add.png'), (12, 60, 780, 200))
check(n > 300, 'list rows render text after add (dark px=%d)' % n)

print('todo_p0: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
