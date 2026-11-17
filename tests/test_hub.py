import sys, tempfile, unittest, struct, json, threading, time
from pathlib import Path
from unittest.mock import patch
from urllib.request import Request, urlopen
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import server as h

def binary(chip=9):
    b=bytearray(512);b[0]=0xe9
    struct.pack_into('<H',b,12,chip);struct.pack_into('<I',b,32,0xabcd5432)
    b[176:208]=bytes(range(32));return bytes(b)
class HubTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.manager=h.DeviceManager(Path(self.tmp.name)/'devices.json')
        self.old={'device_id':'ABC','chip_id':9,'kind':'security','version':1,'elf_sha256':'old'}
        self.rec=self.manager.register({'name':'Security'},self.old,'http://192.168.1.50')
        h.JOBS.clear()
    def tearDown(self):self.tmp.cleanup()
    def test_reject_non_application(self):
        with self.assertRaises(ValueError):h.FirmwareImage(b'wrong'*100)
    def test_image_target_and_hash(self):
        f=h.FirmwareImage(binary());self.assertEqual(f.metadata['chip_id'],9)
        self.assertEqual(f.metadata['elf_sha256'],bytes(range(32)).hex())
    def test_private_address_only(self):
        self.assertEqual(h.base_url('192.168.1.50'),'http://192.168.1.50')
        for value in ('127.0.0.1','8.8.8.8','http://192.168.1.1/other','http://a:b@192.168.1.1'):
            with self.assertRaises(ValueError):h.base_url(value)
    def test_identity_and_chip_guard(self):
        with self.assertRaises(ValueError):h.check_identity(self.rec,{**self.old,'device_id':'OTHER'})
        with self.assertRaises(ValueError):h.check_identity(self.rec,self.old,{'chip_id':0})
    def test_device_polymorphism_and_persistence(self):
        self.assertIsInstance(self.manager.get(self.rec['id']),h.SecurityNode)
        cl={**self.old,'device_id':'DEF','chip_id':0,'kind':'climate'}
        r=self.manager.register({'name':'Climate'},cl,'http://192.168.1.51')
        node=self.manager.get(r['id']);self.assertIsInstance(node,h.ClimateNode)
        with patch.object(h,'info',return_value={**cl,'temperature_c':31.5}):self.assertEqual(node.status()['temperature_c'],31.5)
        loaded=h.DeviceManager(Path(self.tmp.name)/'devices.json');self.assertEqual(len(loaded.records()),2)
        snapshot=node.record;snapshot['name']='changed';self.assertEqual(node.record['name'],'Climate')
    def test_three_board_limit(self):
        for i in (2,3):self.manager.register({}, {**self.old,'device_id':str(i)},f'http://192.168.1.{i}')
        with self.assertRaises(ValueError):self.manager.register({}, {**self.old,'device_id':'4'},'http://192.168.1.4')
    def test_worker_verifies_exact_running_build(self):
        meta=h.image_meta(binary());h.JOBS['test']={'stage':'checking'}
        calls=[]
        def fake_fetch(url,data=None,headers=None,timeout=4):calls.append((url,data));return b'OK'
        with patch.object(h,'info',side_effect=[self.old,{**self.old,'version':2,'elf_sha256':meta['elf_sha256']}]),patch.object(h,'fetch',side_effect=fake_fetch),patch.object(h.time,'sleep'):
            h.deploy('test',self.rec,binary(),meta)
        self.assertEqual(h.JOBS['test']['stage'],'done');self.assertTrue(h.JOBS['test']['verified'])
        self.assertEqual(calls[0][0],self.rec['url']+'/update');self.assertIn(binary(),calls[0][1])
    def test_stale_build_not_success(self):
        h.JOBS['test']={'stage':'checking'}
        with patch.object(h,'info',return_value=self.old),patch.object(h,'fetch',return_value=b'OK'),patch.object(h.time,'sleep'):
            h.deploy('test',self.rec,binary(),h.image_meta(binary()))
        self.assertEqual(h.JOBS['test']['stage'],'unverified')
    def test_http_ui_and_token_guard(self):
        srv=h.ThreadingHTTPServer(('127.0.0.1',0),h.Handler);old=h.PORT;h.PORT=srv.server_port
        t=threading.Thread(target=srv.serve_forever,daemon=True);t.start()
        try:
            base=f'http://127.0.0.1:{srv.server_port}'
            with urlopen(base) as r:self.assertIn(b'NEXUS',r.read())
            with urlopen(base+'/app.js') as r:self.assertEqual(r.status,200)
            from urllib.error import HTTPError
            with self.assertRaises(HTTPError) as e:urlopen(Request(base+'/api/status',data=b'{}'))
            self.assertEqual(e.exception.code,403)
            with patch.object(h,'manager',self.manager),patch.object(h,'info',return_value=self.old):
                req=Request(base+'/api/status',data=json.dumps({'id':self.rec['id']}).encode(),headers={'X-Nexus-Token':h.TOKEN})
                with urlopen(req) as r:self.assertEqual(json.load(r)['device_id'],'ABC')
        finally:srv.shutdown();srv.server_close();h.PORT=old
if __name__=='__main__':unittest.main()
