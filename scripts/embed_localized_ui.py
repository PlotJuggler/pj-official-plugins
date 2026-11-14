#!/usr/bin/env python3
"""Embed plugin-owned display catalogs without a Qt dependency."""
import argparse
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


def localize(xml, entries, ui_path):
    root = ET.fromstring(xml)
    lookup = {}
    for e in entries:
        if e['kind'] != 'exact' or e['ui'] != ui_path:
            continue
        key = (e['widget'], e['property'], e['source'])
        if key in lookup and lookup[key] != e['translation']:
            raise ValueError(f'conflicting translation for {key}')
        if sorted(re.findall(r'%[1-9][0-9]*',e['source'])) != sorted(re.findall(r'%[1-9][0-9]*',e['translation'])):
            raise ValueError(f'placeholder mismatch for {key}')
        lookup[key] = e['translation']
    count = 0
    def visit(node, widget=None):
        nonlocal count
        if node.tag == 'widget':
            widget = node.get('name')
        if node.tag in ('property', 'attribute'):
            value = node.find('string')
            if value is not None and value.get('notr') != 'true':
                key = (widget, node.get('name'), value.text or '')
                if key in lookup:
                    value.text = lookup[key]
                    count += 1
        for child in node:
            visit(child, widget)
    visit(root)
    return ET.tostring(root, encoding='unicode'), count


def generate(ui, catalogs, output, variable):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*',variable):
        raise ValueError('invalid C++ variable name')
    with ui.open(encoding='utf-8', newline='') as stream:
        english = stream.read()
    arrays = [english]
    for catalog in catalogs:
        data = json.loads(catalog.read_text(encoding='utf-8'))
        relative = ui.relative_to(catalog.parent.parent).as_posix()
        arrays.append(localize(english, data['entries'], relative)[0])
    lines=['#pragma once', '#include "display_language.hpp"']
    for suffix, content in zip(('en','cn','tw'), arrays):
        values=','.join('char(%d)' % b for b in content.encode('utf-8'))
        lines.append(f'inline constexpr char {variable}_{suffix}[] = {{{values},0}};')
    lines.append(f'inline const char* const {variable} = pj::localization::text({variable}_en, {variable}_cn, {variable}_tw);')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(lines)+'\n',encoding='utf-8')


if __name__ == '__main__':
    p=argparse.ArgumentParser()
    for name in ('ui','cn','tw','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--variable',required=True)
    a=p.parse_args()
    generate(a.ui,[a.cn,a.tw],a.output,a.variable)
