import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('embed',ROOT/'scripts/embed_localized_ui.py')
embed=importlib.util.module_from_spec(spec);spec.loader.exec_module(embed)

class Localization(unittest.TestCase):
    def test_exact_property_and_notr_only(self):
        xml='<ui><widget name="label"><property name="text"><string>Open</string></property><property name="toolTip"><string notr="true">Open</string></property><widget name="user"><property name="text"><string>Open</string></property></widget></widget></ui>'
        entries=[dict(kind='exact',ui='a.ui',widget='label',property=p,source='Open',translation='打开') for p in ('text','toolTip')]
        result,count=embed.localize(xml,entries,'a.ui')
        self.assertEqual(count,1);self.assertEqual(result.count('Open'),2)
    def test_english_crlf_preserved(self):
        with tempfile.TemporaryDirectory() as t:
            t=Path(t);(t/'locales').mkdir()
            ui=t/'sample.ui';xml=b'<ui>\r\n<widget name="x"/>\r\n</ui>\r\n'
            ui.write_bytes(xml)
            for lang in ('CN','TW'):(t/'locales'/f'zh_{lang}.json').write_text('{"entries":[]}')
            embed.generate(ui,[t/'locales/zh_CN.json',t/'locales/zh_TW.json'],t/'ui.hpp','kUi')
            header=(t/'ui.hpp').read_text()
            self.assertIn('char(13),char(10)',header)
            array=header.split('kUi_en[] = {',1)[1].split('};',1)[0]
            import re
            self.assertEqual(bytes(int(x) for x in re.findall(r'char\((\d+)\)',array)),xml)

    def test_bad_placeholder_rejected(self):
        with self.assertRaises(ValueError):embed.localize('<ui/>',[dict(kind='exact',ui='a',widget='x',property='text',source='%1',translation='%2')],'a')
    def test_catalogs_and_real_ui(self):
        count=0
        for cn in ROOT.glob('*/locales/zh_CN.json'):
            entries=json.loads(cn.read_text())['entries']
            tw=json.loads((cn.parent/'zh_TW.json').read_text())['entries']
            self.assertEqual([(e['kind'],e['ui'],e['widget'],e['property'],e['source']) for e in entries],[(e['kind'],e['ui'],e['widget'],e['property'],e['source']) for e in tw])
            for ui in sorted({e['ui'] for e in entries if e['ui']}):
                source=cn.parent.parent/ui
                self.assertTrue(source.exists(),str(source))
                original=ET.fromstring(source.read_text())
                for data in (entries,tw):
                    result,n=embed.localize(source.read_text(),data,ui)
                    tree=ET.fromstring(result)
                    self.assertEqual([(e.tag,e.attrib) for e in original.iter()],[(e.tag,e.attrib) for e in tree.iter()])
                    self.assertGreaterEqual(n,0)
                    if ui != "transform_editor_help.ui":self.assertGreater(n,0)
                count+=1
        self.assertEqual(count,24)
    def test_compiled_resources_and_fallback(self):
        with tempfile.TemporaryDirectory() as t:
            t=Path(t);module=ROOT/'data_load_csv';ui=module/'dataload_csv.ui'
            embed.generate(ui,[module/'locales/zh_CN.json',module/'locales/zh_TW.json'],t/'ui.hpp','kUi')
            (t/'test.cpp').write_text('#include "ui.hpp"\n#include <iostream>\nint main(){std::cout<<kUi;}\n')
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-pedantic-errors','-I'+str(ROOT/'common/localization'),str(t/'test.cpp'),'-o',str(t/'test')],check=True)
            original=ui.read_text()
            for language in ('en_US','zh_CN','zh_TW','unknown',''):
                env=dict(os.environ,PLOTJUGGLER_UI_LANGUAGE=language)
                actual=subprocess.check_output([str(t/'test')],env=env,text=True)
                if language in ('en_US','unknown',''):self.assertEqual(actual,original)
                else:self.assertNotEqual(actual,original);ET.fromstring(actual)

if __name__=='__main__':unittest.main()
