"""Bounded local resource/ABI artifact check; not device acceptance."""
import hashlib
import pathlib
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
def audit(path):
    ns = {'q':'http://www.qnx.com/schemas/application/1.0'}
    descriptor = ET.parse(ROOT / 'bar-descriptor.xml').getroot()
    with zipfile.ZipFile(path) as archive:
        if sum(item.file_size for item in archive.infolist()) > 32*1024*1024:
            raise ValueError('BBattery uncompressed resources exceed 32 MiB')
        manifest = archive.read('META-INF/MANIFEST.MF').decode('utf-8').replace('\r\n ', '').replace('\n ', '')
        assert 'Package-Version: 0.1.0.12' in manifest
        assert 'Package-Name: top.blaccat.BBattery' in manifest
        for asset in descriptor.findall('q:asset', ns):
            source = ROOT / asset.attrib['path']
            for file in source.rglob('*') if source.is_dir() else [source]:
                if not file.is_file():
                    continue
                name = 'native/' + (asset.text + '/' + file.relative_to(source).as_posix() if source.is_dir() else asset.text)
                assert archive.read(name) == file.read_bytes(), name + ' differs from source'
        for binary in ('native/bbattery','native/batteryd'):
            body = archive.read(binary)
            assert body[:6] == b'\x7fELF\x01\x01' and struct.unpack_from('<H', body,18)[0] == 40, binary
            assert b'libcpp.so.4' in body and b'libstdc++' not in body, binary + ' ABI'
        assert not any(name.endswith(('.key','.pem','.p12','settings.ini','.sqlite')) for name in archive.namelist())
    return {'path':str(pathlib.Path(path).resolve()),'sha256':hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest(),'result':'PASS'}
if __name__ == '__main__':
    print(audit(sys.argv[1]))
