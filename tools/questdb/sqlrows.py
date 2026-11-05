"""Tiny streaming reader for the multi-row INSERT dumps of the TDB: table rows as python lists."""
import re
def columns(path, table):
    c = None
    for l in open(path, errors='replace'):
        if l.startswith('CREATE TABLE `%s`' % table): c = []; continue
        if c is not None:
            if l.startswith('  `'): c.append(l.split('`')[1])
            elif l.startswith(')'): return c
    return None
def _values(s, i):
    """parse one (...) tuple starting at s[i]=='(' -> (list, next index)"""
    i += 1; row = []; n = len(s)
    while True:
        c = s[i]
        if c == "'":
            j = i + 1; buf = []
            while True:
                d = s[j]
                if d == '\\': buf.append(s[j+1]); j += 2
                elif d == "'":
                    if s[j+1:j+2] == "'": buf.append("'"); j += 2
                    else: break
                else: buf.append(d); j += 1
            row.append(''.join(buf)); i = j + 1
        else:
            j = i
            while s[j] not in ',)': j += 1
            t = s[i:j]
            if t == 'NULL': row.append(None)
            else:
                try: row.append(int(t))
                except ValueError:
                    try: row.append(float(t))
                    except ValueError: row.append(t)
            i = j
        if s[i] == ',': i += 1; continue
        if s[i] == ')': return row, i + 1
def rows(path, table):
    head = 'INSERT INTO `%s` ' % table
    for l in open(path, errors='replace'):
        if not l.startswith(head): continue
        i = l.index('VALUES') + 6
        while l[i] != '(': i += 1
        while True:
            row, i = _values(l, i)
            yield row
            while i < len(l) and l[i] in ', \n': i += 1
            if i >= len(l) or l[i] != '(': break
