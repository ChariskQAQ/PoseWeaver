"""Lists tr("...") texts in the editor sources that have no entry in src/editor/i18n_en.inc.

Usage: python scripts/check_i18n.py [--keys]   (--keys prints every key instead, in source order)
Exit code 1 when something is missing.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LITERAL = r'"(?:[^"\\\n]|\\.)*"'
# tr( followed by one or more adjacent string literals and the closing parenthesis.
TR_CALL = re.compile(r'\btr\(\s*((?:' + LITERAL + r'\s*)+)\)')
ENTRY = re.compile(r'^\{\s*(' + LITERAL + r')\s*,\s*(' + LITERAL + r')\s*\},', re.M)


def join_literals(run):
    """Raw (still escaped) text of adjacent literals, as the compiler concatenates them."""
    return ''.join(m[1:-1] for m in re.findall(LITERAL, run))


def source_keys():
    keys = []
    files = sorted(glob.glob(os.path.join(ROOT, 'src', 'editor', '*.cpp')) + glob.glob(os.path.join(ROOT, 'src', 'editor', '*.h')))
    for path in files:
        text = open(path, encoding='utf-8').read()
        for m in TR_CALL.finditer(text):
            key = join_literals(m.group(1)).split('##', 1)[0]
            if key and key not in keys:
                keys.append(key)
    return keys


def table_keys():
    text = open(os.path.join(ROOT, 'src', 'editor', 'i18n_en.inc'), encoding='utf-8').read()
    return {m.group(1)[1:-1] for m in ENTRY.finditer(text)}


if __name__ == '__main__':
    keys = source_keys()
    if '--keys' in sys.argv:
        print('\n'.join(keys))
        sys.exit(0)
    have = table_keys()
    missing = [k for k in keys if k not in have]
    unused = sorted(have - set(keys))
    for k in missing:
        print('missing:', k)
    for k in unused:
        print('unused: ', k)
    print(f'{len(keys)} texts, {len(missing)} missing, {len(unused)} unused')
    sys.exit(1 if missing else 0)
