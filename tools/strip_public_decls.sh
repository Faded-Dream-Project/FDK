#!/bin/sh
# strip_public_decls.sh — the D6 declaration extractor (both gate sides
# share it, so the diff is apples-to-apples): strip block/line comments
# (a raw grep caught fdk_theme_token inside a prose comment as a
# phantom symbol), then keep every fdk_* name followed by '(' —
# function declarations; types never carry one.
# Usage: strip_public_decls.sh <header> [<header> ...]  -> sorted unique names on stdout
awk 'BEGIN { inc = 0 }
{
    line = $0; out = ""; i = 1; n = length(line);
    while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2);
        if (inc) { if (d == "*/") { inc = 0; i += 2; continue } i++; continue }
        if (d == "/*") { inc = 1; i += 2; continue }
        if (d == "//") { break }
        out = out c; i++
    }
    print out
}' "$@" | grep -hoE '\bfdk_[a-z0-9_]+[[:space:]]*\(' | tr -d ' (' | sort -u
