#!/usr/bin/env bash
# Colour check for zterminal: 16 ANSI colours, the 256-colour palette and a
# 24-bit (truecolor) gradient. Run it inside the terminal and compare by eye.
set -u
reset=$'\e[0m'

printf '16 colors:  '
for i in $(seq 0 15); do
  if [ "$i" -lt 8 ]; then printf '\e[4%dm  ' "$i"; else printf '\e[10%dm  ' "$((i - 8))"; fi
done
printf '%s\n' "$reset"

printf '256 colors:\n'
for row in 0 1 2 3 4 5 6 7; do
  printf '  '
  for col in $(seq 0 31); do
    printf '\e[48;5;%dm ' "$((row * 32 + col))"
  done
  printf '%s\n' "$reset"
done

cols=${COLUMNS:-$(tput cols 2>/dev/null || echo 80)}
width=$((cols - 2))
printf 'truecolor:\n  '
for i in $(seq 0 $((width - 1))); do
  # hue sweep red -> yellow -> green -> cyan -> blue -> magenta
  h=$((i * 1530 / width)); seg=$((h / 255)); f=$((h % 255))
  case $seg in
    0) r=255; g=$f; b=0 ;;
    1) r=$((255 - f)); g=255; b=0 ;;
    2) r=0; g=255; b=$f ;;
    3) r=0; g=$((255 - f)); b=255 ;;
    4) r=$f; g=0; b=255 ;;
    *) r=255; g=0; b=$((255 - f)) ;;
  esac
  printf '\e[48;2;%d;%d;%dm ' "$r" "$g" "$b"
done
printf '%s\n  ' "$reset"
for i in $(seq 0 $((width - 1))); do
  v=$((i * 255 / (width - 1)))
  printf '\e[48;2;%d;%d;%dm ' "$v" "$v" "$v"
done
printf '%s\n' "$reset"
printf 'styles: \e[1mbold\e[0m \e[3mitalic\e[0m \e[4munderline\e[0m \e[7mreverse\e[0m \e[9mstrike\e[0m \e[38;2;255;140;0mtruecolor-fg\e[0m \e[38;5;45m256-fg\e[0m\n'
printf 'TERM=%s COLORTERM=%s\n' "${TERM:-}" "${COLORTERM:-}"
