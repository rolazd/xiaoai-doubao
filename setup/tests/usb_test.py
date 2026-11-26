#!/usr/bin/env python3
"""Offline subprocess tests: only generated fake ioreg/update executables are used."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import firmware

class USBTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        shutil.copytree(Path(__file__).resolve().parents[1],self.root/'setup',ignore=shutil.ignore_patterns('__pycache__','tests'))
        self.local=self.root/'.local';self.local.mkdir();(self.local/'extracted').mkdir()
        boot=b'ANDROID!'+struct.pack('<10I',0,0,0,0,0,0,0,2048,0,0)
        boot+=b'\0'*(2560-len(boot))
        (self.local/'extracted/boot.img.encrypt').write_bytes(boot)
        (self.local/'extracted/root.squashfs').write_bytes(b'STOCK')
        (self.local/'root-patched.squashfs').write_bytes(b'PATCH')
        (self.local/'patch.json').write_text(json.dumps({'sha256':hashlib.sha256(b'PATCH').hexdigest()}))
        self.mode='ok';self.write('mode','ok')
        self.ioreg=self.root/'ioreg';self.update=self.root/'update'
        self.ioreg.write_text('''#!/usr/bin/env python3
from pathlib import Path
r=Path(__file__).parent
n=int((r/'count').read_text()) if (r/'count').exists() else 0
(r/'count').write_text(str(n+1))
m=(r/'mode').read_text()
def emit(port,vid,pid):
 print('+-o Device@'+port+' <class IOUSBHostDevice>')
 print('  "idVendor" = '+str(vid));print('  "idProduct" = '+str(pid))
if m=='two':
 emit('abc001',0x2717,0xd002);emit('abc002',0x2717,0xd002)
elif (r/'reset').exists():
 if m!='reset_timeout':emit('abc001',0x2717,0xd002)
elif n==0:emit('abc001',0x2717,0xd002)
else:emit('abc002' if m=='wrong' else 'abc001',0x1b8e,0xc003)
''')
        self.update.write_text('''#!/usr/bin/env python3
from pathlib import Path
import sys
r=Path(__file__).parent;a=sys.argv[1:];m=(r/'mode').read_text()
with (r/'calls').open('a') as f:f.write(' '.join(a)+'\\n')
if a[0]=='identify':
 print('This firmware version is fake',file=sys.stderr if m=='identify_stderr' else sys.stdout)
 if m=='identify_err':print('ERR: identify')
 if m=='identify_no_worldcup':print('No [WorldCup device]')
 if m=='identify_fail':print('fail')
 if m=='identify_nonzero':sys.exit(1)
elif a[0]=='bulkcmd':
 c=a[1].strip()
 if c=='reset':
  (r/'reset').touch()
  if m=='reset_err':print('ERR: bulkcmd failed!');sys.exit(197)
  sys.exit(0)
 if m.endswith('first_io') and not (r/'warm').exists():
  (r/'warm').touch();print('[LUSB]usbReadFile len=512,ret=-5 error_msg=Input/output error');print('ERR: AmlUsbBulkCmd failed!');sys.exit(0)
 boot1=m.startswith('boot1')
 ok=c=='test 1 = 1' or ('boot0' in c and not boot1) or ('boot1' in c and boot1)
 print('bulkInReply success' if ok else 'failed')
elif a[0]=='mread':
 part=a[2];size=int(a[4],16)
 if part.startswith('boot'):data=(r/'.local/extracted/boot.img.encrypt').read_bytes()
 elif (r/'written').exists() and part in (r/'written').read_text().splitlines():data=b'WRONG' if m in ('mismatch','boot1_mismatch') else b'PATCH'
 else:data=b'STOCK'
 if m.startswith('boot1') and part=='boot0':data=data[:-1]+b'X'
 if m=='boot1_stock_mismatch' and part=='system1':data=b'OTHER'
 if m=='backup_mismatch' and a[5].endswith('.2.bin'):data=b'BAD'+data[3:]
 Path(a[5]).write_bytes((data+b'\\0'*size)[:size])
elif a[0]=='partition':
 with (r/'written').open('a') as f:f.write(a[1]+'\\n')
''')
        for p in (self.ioreg,self.update):p.chmod(0o700)
        (self.local/'tool.json').write_text(json.dumps({'sha256':hashlib.sha256(self.update.read_bytes()).hexdigest()}))
    def tearDown(self):self.tmp.cleanup()
    def write(self,n,s):(self.root/n).write_text(s)
    def run_step(self,step,*args,ok=True):
        cmd=[sys.executable,str(self.root/'setup/firmware.py'),str(step),'--ioreg',str(self.ioreg),'--update',str(self.update),*args]
        p=subprocess.run(cmd,capture_output=True,text=True)
        self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr)
        self.assertTrue(p.stdout.splitlines()[-2].startswith('OK:' if ok else 'STOP:'),p.stdout)
        self.assertTrue(p.stdout.splitlines()[-1].startswith('NEXT:'),p.stdout)
        return p
    def backup(self):
        self.run_step(5,'--execute');self.run_step(6,'--execute')
    def test_success(self):
        self.backup();self.run_step(7,'--i-accept-brick-risk');self.assertTrue((self.root/'reset').exists())
        calls=(self.root/'calls').read_text();self.assertEqual(calls.count('partition system0'),1);self.assertEqual(calls.count('reset'),1)
        self.assertEqual((self.root/'written').read_text(),'system0\n')
        self.assertIn('mread store system0 normal 0x5 ',calls)
    def test_two_normal(self):self.write('mode','two');self.run_step(5,'--execute',ok=False)
    def test_identify_output_and_log(self):
        for mode,ok,raw in (
            ('identify_stderr',True,b'This firmware version is fake\n'),
            ('identify_err',False,b'This firmware version is fake\nERR: identify\n'),
            ('identify_no_worldcup',False,b'This firmware version is fake\nNo [WorldCup device]\n'),
            ('identify_fail',True,b'This firmware version is fake\nfail\n'),
            ('identify_nonzero',False,b'This firmware version is fake\n'),
        ):
            with self.subTest(mode=mode):
                self.write('mode',mode)
                (self.root/'count').unlink(missing_ok=True)
                (self.local/'usb.json').unlink(missing_ok=True)
                log=self.local/'identify.log'
                log.write_bytes(b'previous\n');log.chmod(0o644)
                self.run_step(5,'--execute',ok=ok)
                self.assertEqual(log.read_bytes(),b'previous\n'+raw)
                self.assertEqual(log.stat().st_mode & 0o777,0o600)
                self.assertEqual((self.local/'usb.json').exists(),ok)
    def test_accessory_permission_stop(self):
        self.write('mode','identify_no_worldcup')
        result=self.run_step(5,'--execute',ok=False)
        self.assertEqual(result.stdout.splitlines()[-2], 'STOP: 刷机设备没能连上 Mac：多半是 macOS 弹出了"允许配件连接"。请在 系统设置→隐私与安全性 把"允许配件连接"临时改成解锁时自动允许，或下次弹窗时点允许，然后重来')
        self.assertEqual((self.root/'calls').read_text().splitlines(), ['identify 7'])
        self.assertFalse((self.local/'usb.json').exists())

    def test_wrong_port(self):self.write('mode','wrong');self.run_step(5,'--execute',ok=False)
    def test_backup_boot1(self):
        self.run_step(5,'--execute');self.write('mode','boot1')
        result=self.run_step(6,'--execute')
        self.assertEqual(result.stdout.splitlines()[-3], '注意：音箱在用 boot1/system1，第 7 步将写 system1（已在一台网友的音箱上验证）')
        self.assertEqual(json.loads((self.local/'boot-part.json').read_text()),{'boot_part':'boot1'})
        for part in ('boot0','boot1','system0','system1'):
            copies=[self.local/'backup'/f'{part}.{i}.bin' for i in (1,2)]
            self.assertTrue(all(p.is_file() and p.stat().st_size>0 for p in copies))
            self.assertEqual(*(firmware.sha(p) for p in copies))
        self.assertFalse((self.root/'written').exists())
        self.assertFalse((self.root/'reset').exists())
        self.assertTrue((self.local/'backup.json').exists())
        self.assertFalse(any(line.startswith('partition ') for line in (self.root/'calls').read_text().splitlines()))
    def test_first_bulkcmd_io_error_ignored(self):
        self.run_step(5,'--execute');self.write('mode','boot1_first_io')
        self.run_step(6,'--execute')
        self.assertEqual(json.loads((self.local/'boot-part.json').read_text()),{'boot_part':'boot1'})
        self.assertIn('Input/output error',(self.local/'update.log').read_text())
        self.assertFalse((self.root/'written').exists())
    def test_boot1(self):
        self.write('mode','boot1');self.backup()
        self.run_step(7,'--i-accept-brick-risk')
        self.assertEqual((self.root/'written').read_text(),'system1\n')
        calls=(self.root/'calls').read_text()
        self.assertIn('mread store system1 normal 0x5 ',calls)
        self.assertEqual(calls.count('reset'),1)
        self.assertTrue((self.root/'reset').exists())
    def test_boot1_success(self):
        self.write('mode','boot1');self.backup()
        self.run_step(7,'--i-accept-brick-risk','--experimental-boot1')
        self.assertEqual((self.root/'written').read_text(),'system1\n')
        calls=(self.root/'calls').read_text()
        self.assertIn('mread store system1 normal 0x5 ',calls)
        self.assertEqual(calls.count('reset'),1)
        self.assertTrue((self.root/'reset').exists())
    def test_boot_part_changed(self):
        self.backup();self.write('mode','boot1')
        result=self.run_step(7,'--i-accept-brick-risk','--experimental-boot1',ok=False)
        self.assertIn('boot_part 与第 6 步记录不一致',result.stdout)
        self.assertFalse((self.root/'written').exists())
        self.assertFalse((self.root/'reset').exists())
    def test_boot1_part_changed_to_boot0(self):
        self.write('mode','boot1');self.backup();self.write('mode','ok')
        result=self.run_step(7,'--i-accept-brick-risk','--experimental-boot1',ok=False)
        self.assertIn('boot_part 与第 6 步记录不一致',result.stdout)
        self.assertFalse((self.root/'written').exists())
    def test_boot1_stock_mismatch(self):
        self.write('mode','boot1_stock_mismatch');self.run_step(5,'--execute')
        result=self.run_step(6,'--execute',ok=False)
        self.assertIn('system1 prefix differs from OTA',result.stdout)
        self.assertFalse((self.local/'backup.json').exists())
        self.assertFalse((self.root/'written').exists())
    def test_boot1_readback_mismatch(self):
        self.write('mode','boot1');self.backup();self.write('mode','boot1_mismatch')
        result=self.run_step(7,'--i-accept-brick-risk','--experimental-boot1',ok=False)
        self.assertIn('readback hash mismatch',result.stdout)
        self.assertEqual((self.root/'written').read_text(),'system1\n')
        self.assertFalse((self.root/'reset').exists())
    def test_readback_mismatch(self):
        self.backup();self.write('mode','mismatch');self.run_step(7,'--i-accept-brick-risk',ok=False);self.assertFalse((self.root/'reset').exists())
    def test_backup_mismatch(self):
        self.run_step(5,'--execute');self.write('mode','backup_mismatch');self.run_step(6,'--execute',ok=False)
        self.assertFalse((self.root/'reset').exists())
    def test_dry_run(self):
        result=self.run_step(7)
        self.assertIn('update partition <system0|system1> ',result.stdout)
        for active in ('boot0','boot1'):
            with self.subTest(active=active):
                (self.local/'boot-part.json').write_text(json.dumps({'boot_part':active}))
                result=self.run_step(7,'--experimental-boot1')
                self.assertIn('update partition '+firmware.SYSTEM[active]+' ',result.stdout)
        self.assertFalse((self.root/'calls').exists());self.assertFalse((self.root/'count').exists())
    def test_experimental_boot1_only_step7(self):
        self.run_step(6,'--execute','--experimental-boot1',ok=False)
        self.assertFalse((self.root/'calls').exists())
    def test_reset_only(self):
        self.run_step(5,'--execute');self.run_step(7,'--reset-only','--execute');self.assertTrue((self.root/'reset').exists());self.assertFalse((self.root/'written').exists())

    def test_reset_err_then_normal(self):
        self.run_step(5,'--execute');self.write('mode','reset_err')
        self.run_step(7,'--reset-only','--execute')
        self.assertIn('ERR:',(self.local/'update.log').read_text())
        self.assertEqual((self.root/'calls').read_text().splitlines()[-1],'bulkcmd      reset')
    def test_reset_timeout(self):
        self.run_step(5,'--execute');self.write('mode','reset_timeout')
        args=SimpleNamespace(reset_only=True,ioreg=str(self.ioreg),update=self.update)
        with patch.object(firmware,'LOCAL',self.local), patch.object(firmware,'time',SimpleNamespace(monotonic=Mock(side_effect=[0,0,59.8,60]),sleep=Mock())) as clock:
            with self.assertRaisesRegex(RuntimeError,'音箱没有正常回到普通模式；先不要拔插，到 GitHub 提 Issue'):
                firmware.prepare(7,args)
            self.assertEqual(clock.sleep.call_count,2)
            clock.sleep.assert_called_with(.2)

if __name__=='__main__':unittest.main()
