#!/usr/bin/env python3
"""Prueft die Uebersetzungen der Oberflaeche (Deutsch -> Englisch).

- Jeder mit T(), N_(), TF() oder TC() markierte Text muss in src/core/lang_en.cpp stehen.
- Platzhalter ({0}, %d, %s ...) muessen in beiden Sprachen gleich sein.
- Doppelte Eintraege in der Tabelle sind ein Fehler, ungenutzte nur ein Hinweis.
- Nicht markierte Texte, die deutsch aussehen, sind ein Fehler. Ausnahmen werden in der
  Zeile mit dem Kommentar  // lang:ok  gekennzeichnet.

Aufruf: python3 tools/check-lang.py [--verbose]
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SRC = os.path.join(ROOT, 'src')
TABLE = os.path.join(SRC, 'core', 'lang_en.cpp')

# Dateien ohne Oberflaechentexte (Schluessel, Tests, Tabellen)
SKIP_HEURISTIC = {
    'core/lang.cpp', 'core/lang_en.cpp', 'core/lang.h', 'indextest.cpp', 'settings.cpp',
    'core/query.cpp', 'core/textutil.cpp', 'core/patterns.cpp',
}

GERMAN_WORDS = set('''
und oder nicht der die das dem den des ein eine einen einer kein keine keinen wird werden wurde
wurden ist sind mit von vom zur zum nach für fuer auf aus bei beim ohne über ueber unter bis
datei dateien ordner bild bilder fehler kann können konnte seite seiten alle alles heute gestern
letzte letzten größe groesse datum typ speichern abbrechen öffnen oeffnen element elemente
ausgewählt merkliste sammlung sammlungen einstellungen hinweis erstellen löschen loeschen
kopieren einfügen einfuegen umbenennen eigenschaften zuschneiden vergleichen bitte noch schon
hier dieser diese dieses neu neue neuen gespeichert geladen läuft laeuft vorschau vorschaubild
vorschaubilder auflösung aufloesung aufgenommen dauer endung kamera quelle muster unterordner
zurück zurueck weiter fertig jetzt nur auch wählen waehlen anzeigen ansicht sortieren
'''.split())

ESC = re.compile(r'\\(u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|x[0-9a-fA-F]+|[0-7]{1,3}|.)', re.S)


def decode(body):
    def rep(m):
        e = m.group(1)
        if e[0] in 'uU':
            return chr(int(e[1:], 16))
        if e[0] == 'x':
            return chr(int(e[1:], 16))
        if e[0].isdigit():
            return chr(int(e, 8))
        return {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '\\': '\\', '"': '"', "'": "'"}.get(e, e)
    return ESC.sub(rep, body)


def strip_comments(text):
    """Ersetzt Kommentare durch Leerzeichen (Zeilenumbrueche bleiben), Strings bleiben."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


LIT = r'L"(?:[^"\\\n]|\\.)*"'
LITSEQ = r'(?:%s\s*)+' % LIT
ONE_LIT = re.compile(LIT)


def join_lits(seq):
    return ''.join(decode(m.group(0)[2:-1]) for m in ONE_LIT.finditer(seq))


MARK = re.compile(r'\b(T|N_|TF)\(\s*(' + LITSEQ + r')')
MARK_TC = re.compile(r'\bTC\(\s*(' + LITSEQ + r')\s*,\s*(' + LITSEQ + r')')


def placeholders(s):
    curly = sorted(re.findall(r'\{\d\}', s))
    printf = re.findall(r'%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|h|z|I64)?[diouxXfFeEgGcsS%]', s)
    return curly, printf


def parse_table():
    text = strip_comments(open(TABLE, encoding='utf-8').read())
    pairs = []
    for m in re.finditer(r'\{\s*(' + LITSEQ + r')\s*,\s*(' + LITSEQ + r')\s*\}', text):
        line = text.count('\n', 0, m.start()) + 1
        pairs.append((join_lits(m.group(1)), join_lits(m.group(2)), line))
    return pairs


def looks_german(s):
    if re.search('[äöüÄÖÜß]', s):
        return True
    words = re.findall(r'[A-Za-zÄÖÜäöüß]+', s)
    return any(w.lower() in GERMAN_WORDS for w in words)


def main():
    verbose = '--verbose' in sys.argv
    errors, notes = [], []
    pairs = parse_table()
    table = {}
    for de, en, line in pairs:
        if de in table:
            errors.append(f'lang_en.cpp:{line}: doppelter Eintrag: {de!r}')
        table[de] = en
        cd, pd = placeholders(de)
        ce, pe = placeholders(en)
        if cd != ce or pd != pe:
            errors.append(f'lang_en.cpp:{line}: Platzhalter verschieden: {de!r} -> {en!r}')
        if not en.strip() and de.strip():
            errors.append(f'lang_en.cpp:{line}: leere Uebersetzung fuer {de!r}')

    used = set()
    for dirpath, _, files in os.walk(SRC):
        for fn in sorted(files):
            if not fn.endswith(('.cpp', '.h')):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, SRC).replace(os.sep, '/')
            raw = open(path, encoding='utf-8').read()
            text = strip_comments(raw)
            raw_lines = raw.split('\n')
            marked_spans = []
            for m in MARK.finditer(text):
                key = join_lits(m.group(2))
                used.add(key)
                marked_spans.append((m.start(2), m.end(2)))
                if key not in table:
                    line = text.count('\n', 0, m.start()) + 1
                    errors.append(f'{rel}:{line}: fehlt in lang_en.cpp: {key!r}')
            for m in MARK_TC.finditer(text):
                key = join_lits(m.group(1))
                used.add(key)
                marked_spans.append((m.start(1), m.end(2)))
                if key not in table:
                    line = text.count('\n', 0, m.start()) + 1
                    errors.append(f'{rel}:{line}: fehlt in lang_en.cpp (TC): {key!r}')
            if rel in SKIP_HEURISTIC:
                continue
            for m in ONE_LIT.finditer(text):
                if any(a <= m.start() < b for a, b in marked_spans):
                    continue
                s = decode(m.group(0)[2:-1])
                if not looks_german(s):
                    continue
                line = text.count('\n', 0, m.start()) + 1
                if 'lang:ok' in raw_lines[line - 1]:
                    continue
                errors.append(f'{rel}:{line}: nicht markierter deutscher Text: {s!r}')

    for de in table:
        if de not in used:
            notes.append(f'ungenutzter Eintrag: {de!r}')

    for n in notes if verbose else []:
        print('Hinweis:', n)
    for e in errors:
        print('Fehler:', e)
    print(f'{len(table)} Eintraege, {len(used)} markierte Texte, {len(errors)} Fehler, {len(notes)} ungenutzt')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
