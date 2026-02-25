import sys
import re

with open('exprtk_mod_ta.c', 'r', encoding='utf-8') as f:
    text = f.read()

# Replace all "ta.func" with "func" in the entries array
text = re.sub(r'\"ta\.([a-zA-Z0-9_]+)\"', r'"\1"', text)

with open('exprtk_mod_ta.c', 'w', encoding='utf-8') as f:
    f.write(text)
