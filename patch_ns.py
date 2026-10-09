import re

# 1. message.h: add using-directive so Message (global scope) sees carlinkit::Cmd/MAGIC
p = r'C:\tang-carplay\client\cpp\protocol\message.h'
s = open(p, encoding='utf-8').read()
if 'using namespace carlinkit;' not in s:
    # Insert after the protocol_const.h include
    s = s.replace('#include "protocol_const.h"',
                  '#include "protocol_const.h"\n\n'
                  '// Message/Header live at global scope (matches FastCarPlay layout);\n'
                  '// pull the carlinkit constants (Cmd/MAGIC/MAGIC_ENC) into view.\n'
                  'using namespace carlinkit;')
    open(p, 'w', encoding='utf-8').write(s)
    print('message.h: using-directive added')
else:
    print('message.h: already has using-directive')

# 2. Verify no unqualified Cmd/MAGIC remain that would break (they resolve via using)
for pat, name in [(r'\bCmd\b(?!_)', 'Cmd'), (r'\bMAGIC\b(?!_)', 'MAGIC')]:
    hits = [m.start() for m in re.finditer(pat, s)]
    print(f'message.h bare {name}: {len(hits)} (resolve via using-directive)')

# 3. test_protocol.cpp: ensure using-directive present
p2 = r'C:\tang-carplay\client\test\test_protocol.cpp'
s2 = open(p2, encoding='utf-8').read()
if 'using namespace carlinkit;' not in s2:
    s2 = s2.replace('#include "fake_transport.h"',
                    '#include "fake_transport.h"\n\nusing namespace carlinkit;')
    open(p2, 'w', encoding='utf-8').write(s2)
    print('test_protocol.cpp: using-directive added')
else:
    print('test_protocol.cpp: already has using-directive')

# 4. ce_main.cpp: same check
p3 = r'C:\tang-carplay\client\cpp\ce_main.cpp'
s3 = open(p3, encoding='utf-8').read()
if 'using namespace carlinkit;' not in s3:
    # ce_main body is inside #if defined(__MINGW32CE__)...; insert after includes
    s3 = s3.replace('#include <vector>\n',
                    '#include <vector>\n\nusing namespace carlinkit;\n', 1)
    open(p3, 'w', encoding='utf-8').write(s3)
    print('ce_main.cpp: using-directive added')
else:
    print('ce_main.cpp: already has using-directive')

print('DONE')