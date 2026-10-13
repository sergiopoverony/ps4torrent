#!/bin/sh
# Генерирует lang_gen.S и lang_gen.cpp из всех файлов assets/lang/*.json (код языка = имя файла без .json).
# Новый язык: положить xx.json в assets/lang/ и пересобрать (make); править код не нужно.
S=lang_gen.S
C=lang_gen.cpp
echo '/* создано mklang.sh, не править */' > $S
echo '    .section .rodata' >> $S
{
echo '/* создано mklang.sh, не править */'
echo 'struct LangFile { const char* code; const unsigned char* start; const unsigned char* end; };'
} > $C
n=0
for f in assets/lang/*.json; do
  [ -f "$f" ] || continue
  code=$(basename "$f" .json)
  case "$code" in *[!a-zA-Z0-9-]*|'') echo "bad language file name: $f" >&2; exit 1;; esac
  printf '    .balign 16\n    .globl lang_%d_start\n    .globl lang_%d_end\nlang_%d_start:\n    .incbin "%s"\nlang_%d_end:\n    .byte 0\n' $n $n $n "$f" $n >> $S
  echo "extern \"C\" const unsigned char lang_${n}_start[], lang_${n}_end[];" >> $C
  n=$((n+1))
done
echo '#if defined(__linux__)' >> $S
echo '    .section .note.GNU-stack,"",@progbits' >> $S
echo '#endif' >> $S
echo 'extern const LangFile g_langFiles[];' >> $C
echo 'const LangFile g_langFiles[] = {' >> $C
i=0
for f in assets/lang/*.json; do
  [ -f "$f" ] || continue
  code=$(basename "$f" .json)
  echo "    { \"$code\", lang_${i}_start, lang_${i}_end }," >> $C
  i=$((i+1))
done
echo '    { 0, 0, 0 }' >> $C
echo '};' >> $C
echo "extern const int g_langCount;" >> $C
echo "const int g_langCount = $n;" >> $C
