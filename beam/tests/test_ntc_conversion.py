"""元Android命令の独立した実行結果と、製品コードのNTC換算を照合する。"""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class NtcConversion(unittest.TestCase):
    def test_android_reference_and_faults(self):
        ref = json.loads(Path(__file__).with_name('beam-ntc-reference.json').read_text())
        source = r'''
#include <stdio.h>
#include "beam-ntc-conversion.h"
int main(void) {
    for (int b=0;b<2;b++) for(int raw=-1;raw<=1024;raw++) {
        int mc=0,err=beam_ntc_convert(raw,b,&mc);
        printf("%d %d %d %d\n",b,raw,err,mc);
    }
    int a,b,c;
    while(scanf("%d %d %d",&a,&b,&c)==3) {
        int v[]={a,b,c};printf("outside %d\n",beam_ntc_outside(v));
    }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp)/'check.c'; exe = Path(tmp)/'check'
            src.write_text(source)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                            '-I'+str(ROOT/'drivers/hwmon'),str(src),'-o',str(exe)],check=True)
            data=''.join(f"{x['ntc1']*100} {x['ntc2']*100} {x['ntc3']*100}\n" for x in ref['outside'])
            lines=subprocess.check_output([str(exe)],input=data.encode()).decode().splitlines()
        outside=[]
        for line in lines:
            if line.startswith('outside '):
                outside.append(int(line.split()[1])); continue
            b,raw,err,mc=map(int,line.split())
            expected=ref['adc'][raw][b+1] if 0<=raw<1024 else None
            if expected is None:
                self.assertLess(err,0,(b,raw,mc))
            else:
                self.assertEqual((err,mc),(0,expected),(b,raw))
        self.assertEqual(outside,[x['original_outside_decicelsius']*100 for x in ref['outside']])

if __name__=='__main__': unittest.main()
