"""NEXUS local OTA hub. Python 3.10+. Only pyserial is optional (USB discovery)."""
import hashlib, ipaddress, json, os, secrets, socket, struct, threading, time, uuid, webbrowser
from pathlib import Path
from abc import ABC, abstractmethod
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, build_opener, ProxyHandler, HTTPRedirectHandler
from urllib.parse import urlsplit
ROOT=Path(__file__).resolve().parent
STATE=ROOT/'devices.json'
TOKEN=secrets.token_urlsafe(32)
LOCK=threading.RLock()
JOBS={}
PORT=8765
MAX_BIN=16*1024*1024
class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs): raise ValueError('Device redirected the request; refused.')
def fetch(url,data=None,headers=None,timeout=4):
    req=Request(url,data=data,headers=headers or {})
    with build_opener(ProxyHandler({}),NoRedirect()).open(req,timeout=timeout) as r:
        return r.read(2*1024*1024)
def base_url(value):
    value=value.strip()
    if '://' not in value:value='http://'+value
    u=urlsplit(value)
    if u.scheme!='http' or u.username or u.password or u.path not in ('','/') or u.query or u.fragment:
        raise ValueError('Use an HTTP IPv4 address, e.g. 192.168.1.42')
    ip=ipaddress.ip_address(u.hostname or '')
    if ip.version!=4 or not ip.is_private or ip.is_loopback or ip.is_unspecified or ip.is_multicast:
        raise ValueError('Use the board’s private LAN IPv4 address.')
    return 'http://'+str(ip)+(':'+str(u.port) if u.port else '')
def info(url):
    d=json.loads(fetch(url+'/info'))
    if not isinstance(d,dict):raise ValueError('Device /info response is not an object.')
    return d
def image_meta(data):
    if len(data)<288 or data[0]!=0xE9 or struct.unpack_from('<I',data,32)[0]!=0xABCD5432:
        raise ValueError('Choose the application .ino.bin, not a merged image, bootloader, or partitions file.')
    chip=struct.unpack_from('<H',data,12)[0]
    if chip not in (0,9):raise ValueError('This hub currently supports ESP32 and ESP32-S3 application images.')
    return {'chip_id':chip,'chip':'ESP32-S3' if chip==9 else 'ESP32','size':len(data),
      'sha256':hashlib.sha256(data).hexdigest(),'elf_sha256':data[176:208].hex()}
def check_identity(d,current,meta=None):
    if not current.get('device_id') or current.get('device_id')!=d.get('device_id'):
        raise ValueError('Device identity changed or missing. Check the IP and register the correct board again.')
    if current.get('chip_id') not in (0,9):raise ValueError('Install the supplied Nexus-compatible firmware first.')
    if meta and current['chip_id']!=meta['chip_id']:raise ValueError('Firmware chip mismatch. Compile for the selected board.')
def job_update(jid,**kw):
    with LOCK:JOBS[jid].update(kw)
def deploy(jid,d,payload,meta):
    try:
        job_update(jid,stage='checking',message='Verifying selected board identity and chip…')
        before=info(d['url']);check_identity(d,before,meta)
        if before.get('elf_sha256')==meta['elf_sha256']:
            job_update(jid,stage='done',message='This exact application build is already running.',verified=True);return
        boundary='Nexus'+secrets.token_hex(16)
        body=(f'--{boundary}\r\nContent-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\nContent-Type: application/octet-stream\r\n\r\n'.encode()+payload+f'\r\n--{boundary}--\r\n'.encode())
        job_update(jid,stage='uploading',message='Sending firmware over Wi-Fi. Keep the board powered.')
        upload_warning=''
        try:
            answer=fetch(d['url']+'/update',body,{'Content-Type':'multipart/form-data; boundary='+boundary},90).decode().strip()
            if answer!='OK':raise ValueError('Device rejected firmware: '+answer[:200])
        except (OSError,TimeoutError) as e:
            # A reboot can close the connection after the image was accepted. Verify instead of declaring success.
            upload_warning=' Upload connection closed; checking the running image.'
        job_update(jid,stage='verifying',message='Waiting for reboot and verifying application fingerprint.'+upload_warning)
        time.sleep(3)
        for _ in range(25):
            try:
                after=info(d['url']);check_identity(d,after,meta)
                if after.get('elf_sha256')==meta['elf_sha256']:
                    job_update(jid,stage='done',message='Update verified. The selected board is running the uploaded build.',verified=True,version=after.get('version'));return
            except Exception:pass
            time.sleep(2)
        job_update(jid,stage='unverified',message='New build not verified. Check power/network, refresh the board, and confirm its version before retrying.')
    except Exception as e:job_update(jid,stage='failed',message=str(e))
def ports():
    try:
        from serial.tools import list_ports
        return {'ports':[{'port':p.device,'description':p.description,'serial':p.serial_number or ''} for p in list_ports.comports()]}
    except ImportError:return {'ports':[],'note':'Install pyserial for USB identification. OTA by IP works without it.'}
def identify(port):
    import serial
    allowed={p['port'] for p in ports()['ports']}
    if port not in allowed:raise ValueError('Port is no longer connected. Refresh USB ports.')
    # Opening a port can reset some boards. This is an explicit user action only.
    s=serial.Serial();s.port=port;s.baudrate=115200;s.timeout=.3;s.dtr=False;s.rts=False
    s.open()
    try:
        deadline=time.monotonic()+10;next_send=0
        while time.monotonic()<deadline:
            if time.monotonic()>=next_send:s.write(b'NEXUS_INFO\n');next_send=time.monotonic()+1
            line=s.readline().decode(errors='replace').strip()
            if line.startswith('NEXUS_INFO '):return json.loads(line[len('NEXUS_INFO '):])
        raise ValueError('No Nexus identity reply. Close Serial Monitor and install Nexus firmware, or register its Wi-Fi IP manually.')
    finally:s.close()
class Device(ABC):
    """Abstract device contract. Subclasses normalize their own telemetry."""
    def __init__(self, record):
        self._record = dict(record)
    @property
    def record(self):
        return dict(self._record)  # do not expose mutable internal state
    def status(self):
        response = info(self._record['url'])
        check_identity(self._record, response)
        response.update(self.telemetry(response))  # runtime polymorphism
        return response
    @abstractmethod
    def telemetry(self, response):
        """Return display fields for this device kind."""

class SecurityNode(Device):
    def telemetry(self, response):
        return {'kind': 'security', 'distance_mm': response.get('distance_mm'),
                'pot_adc': response.get('pot_adc'), 'led_level': response.get('led_level'),
                'buzzer': response.get('buzzer', False)}

class ClimateNode(Device):
    def telemetry(self, response):
        return {'kind': 'climate', 'temperature_c': response.get('temperature_c'),
                'humidity': response.get('humidity'), 'ir_detected': response.get('ir_detected', False),
                'motor_on': response.get('motor_on', False)}

class DeviceManager:
    """Owns at most three Device objects and persists their connection metadata."""
    def __init__(self, path):
        self._path = path
        self._devices = {}
        if path.exists():
            for record in json.loads(path.read_text(encoding='utf-8')):
                self._devices[record['id']] = self._create(record)
    @staticmethod
    def _create(record):
        return (ClimateNode if record.get('kind') == 'climate' else SecurityNode)(record)
    def records(self):
        with LOCK: return [d.record for d in self._devices.values()]
    def get(self, did):
        with LOCK:
            if did not in self._devices: raise ValueError('Select a registered board.')
            return self._devices[did]
    def _save(self):
        temp = self._path.with_suffix('.tmp')
        temp.write_text(json.dumps(self.records(), indent=2), encoding='utf-8'); temp.replace(self._path)
    def register(self, data, live, url):
        with LOCK:
            record = next((d.record for d in self._devices.values()
                           if d.record['device_id'] == live['device_id']), None)
            if record is None:
                if len(self._devices) >= 3: raise ValueError('Three boards are registered. Remove one first.')
                record = {'id': uuid.uuid4().hex}
            record.update(name=str(data.get('name') or live.get('name') or 'ESP32 node')[:48],
                          url=url, device_id=live['device_id'], chip_id=live['chip_id'],
                          kind=live.get('kind', 'security'), port=data.get('port', ''))
            self._devices[record['id']] = self._create(record)
            self._save()
            return record
    def remove(self, did):
        with LOCK:
            if any(j['stage'] in ('checking','uploading','verifying') for j in JOBS.values()):
                raise ValueError('Wait for the current update.')
            self._devices.pop(did, None); self._save()

class FirmwareImage:
    """Encapsulates application bytes, target chip and build fingerprints."""
    def __init__(self, payload):
        self._payload = bytes(payload)
        self._metadata = image_meta(self._payload)
    @property
    def metadata(self): return dict(self._metadata)
    @property
    def payload(self): return self._payload

class OTAService:
    """Coordinates target selection, validation, transfer and reboot verification."""
    def __init__(self, manager): self._manager = manager  # composition
    def start(self, did, image):
        target = self._manager.get(did).record
        check_identity(target, info(target['url']), image.metadata)
        with LOCK:
            if any(j['stage'] in ('checking','uploading','verifying') for j in JOBS.values()):
                raise ValueError('An update is already in progress.')
            jid = uuid.uuid4().hex
            JOBS[jid] = {'id':jid, 'device':target['name'], 'stage':'checking',
                         'message':'Preparing update', 'meta':image.metadata}
        threading.Thread(target=deploy, args=(jid,target,image.payload,image.metadata), daemon=True).start()
        return {'job':jid, 'meta':image.metadata}

manager = DeviceManager(STATE)
ota_service = OTAService(manager)

class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def output(self,data,status=200,kind='application/json'):
        if kind=='application/json':data=json.dumps(data).encode()
        if isinstance(data,str):data=data.encode()
        self.send_response(status);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(data)))
        self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.end_headers();self.wfile.write(data)
    def valid_host(self):return self.headers.get('Host') in (f'127.0.0.1:{PORT}',f'localhost:{PORT}')
    def do_GET(self):
        if not self.valid_host():self.output({'error':'Invalid host'},403);return
        try:
            path=urlsplit(self.path).path
            if path=='/':self.output((ROOT/'static/index.html').read_text(encoding='utf-8').replace('__TOKEN__',TOKEN),kind='text/html; charset=utf-8')
            elif path in ('/app.js','/style.css'):
                self.output((ROOT/'static'/path[1:]).read_bytes(),kind='text/javascript' if path.endswith('.js') else 'text/css')
            elif path=='/api/devices':
                self.output({'devices':manager.records()})
            elif path=='/api/ports':self.output(ports())
            elif path.startswith('/api/job/'):
                with LOCK:self.output(JOBS.get(path.rsplit('/',1)[1],{'stage':'missing','message':'Job not found'}))
            else:self.output({'error':'Not found'},404)
        except Exception as e:self.output({'error':str(e)},400)
    def do_POST(self):
        if not self.valid_host() or self.headers.get('X-Nexus-Token')!=TOKEN:self.output({'error':'Reload the local dashboard.'},403);return
        try:
            n=int(self.headers.get('Content-Length','0'))
            if n<1 or n>MAX_BIN:raise ValueError('Request must be between 1 byte and 16 MiB.')
            raw=self.rfile.read(n)
            path=urlsplit(self.path).path
            if path.startswith('/api/upload/'):
                self.output(ota_service.start(path.rsplit('/',1)[1], FirmwareImage(raw)));return
            data=json.loads(raw)
            if path=='/api/register':
                url=base_url(data.get('url',''));live=info(url)
                if not live.get('device_id') or live.get('chip_id') not in (0,9):raise ValueError('This is legacy firmware. Use its existing OTA page to install the supplied SmartGuard_Nexus sketch first.')
                found=manager.register(data,live,url)
                self.output({'device':found,'info':live})
            elif path=='/api/status':
                self.output(manager.get(data['id']).status())
            elif path=='/api/remove':
                manager.remove(data['id']);self.output({'ok':True})
            elif path=='/api/identify':self.output(identify(data['port']))
            else:self.output({'error':'Not found'},404)
        except Exception as e:self.output({'error':str(e)},400)
def main():
    server=ThreadingHTTPServer(('127.0.0.1',PORT),Handler)
    print(f'NEXUS OTA Hub running at http://localhost:{PORT}\nKeep this window open. Ctrl+C to stop.')
    if '--no-browser' not in __import__('sys').argv:threading.Timer(.6,lambda:webbrowser.open(f'http://localhost:{PORT}')).start()
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close()
if __name__=='__main__':main()
