# Test fonts

Tests and goldens use only these fonts, never system fonts. All are licensed under the SIL
Open Font License 1.1 (`OFL-*.txt`, one per family as published upstream).

Source: [google/fonts](https://github.com/google/fonts) at commit
`6c1dbe27dcf7d817d4b62cec033ba77fa8e2a5ec`, directory `ofl/<family>/`. Files are renamed
to avoid brackets in paths.

| File | Upstream file | sha256 of the upstream file | Covers |
| --- | --- | --- | --- |
| `NotoSans-VF.ttf` | `notosans/NotoSans[wdth,wght].ttf` | `bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d` | Latin, Greek, Cyrillic; `wght` 100–900, `wdth` 62.5–100 |
| `NotoSansArabic-VF.ttf` | `notosansarabic/NotoSansArabic[wdth,wght].ttf` | `63111b5b2e074dd48cc67692e0a2726d86ee94c1c37fe8598257b7b4e87e869e` | Arabic |
| `NotoSansHebrew-VF.ttf` | `notosanshebrew/NotoSansHebrew[wdth,wght].ttf` | `7ef36a2c3593758cdb622e1bdef4f84523e92fbc3ccc667438dd80ff54c2de88` | Hebrew |
| `NotoSansDevanagari-VF.ttf` | `notosansdevanagari/NotoSansDevanagari[wdth,wght].ttf` | `14ec4af41f27482216d1c2229f417ff9b1425e1babb014e57d1d40d03229853e` | Devanagari |
| `NotoSansSC-Subset.ttf` | `notosanssc/NotoSansSC[wght].ttf`, subset | `a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da` | the characters in `cjk_chars.txt` |

## The CJK subset

The full Noto Sans SC is 17.8 MB, so it is subset to the code points listed in
`cjk_chars.txt`: test and example strings, common hanzi, kana, CJK punctuation and
fullwidth forms. Variations are kept. To add characters, edit `cjk_chars.txt` and
regenerate:

```sh
cmake --build --preset debug --target font_subset
build/debug/tools/font_subset 'NotoSansSC[wght].ttf' tests/fonts/cjk_chars.txt \
    tests/fonts/NotoSansSC-Subset.ttf
```

The output is byte-identical for the same HarfBuzz version and character list.

**License note.** Noto Sans SC's OFL declares the Reserved Font Name "Source" (from Adobe's
Source Han Sans). The subset is a Modified Version under the OFL, so it must not use that
name. Its family name stays "Noto Sans SC", which does not contain it.
