import sys
p = r'C:\tang-carplay\client\cpp\ce_main.cpp'
data = open(p, 'rb').read()
# broken literal inserted by Add-Content: backtick + 'n' as two chars (0x60 0x6E)
bad = b'`n' + b'#endif // _WIN32_WCE'
if bad in data:
    data = data.replace(bad, b'')
    open(p, 'wb').write(data)
    print('cleaned')
else:
    print('not found')
# show last 4 lines
text = data.decode('utf-8', 'replace')
for l in text.splitlines()[-4:]:
    print(' ', l[:80])
print('total lines:', len(text.splitlines()))
