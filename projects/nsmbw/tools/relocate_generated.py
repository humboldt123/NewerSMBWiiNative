"""Make a generated_nsmbw-<version> folder translated on another machine build on this one.

The translator writes absolute paths (build_shards/shards.cmake, the .incbin lines of
data_sections_init_blobs.S), and older translators wrote that .S for one object format only. This
rewrites the paths to this checkout and the .S to the portable form current translators emit
(Translator.Core/IO/AssemblyBlobWriter.cs), so code translated on Windows builds on macOS.

  python3 -I projects/nsmbw/tools/relocate_generated.py generated_nsmbw-E2N C:/path/to/their/checkout
"""
import os
import re
import sys

PORTABLE_SECTION = (
    '#if defined(__APPLE__)\n.section __TEXT,__const\n#define BLOB_SYMBOL(name) _##name\n'
    '#elif defined(_WIN32)\n.section .rdata,"dr"\n#define BLOB_SYMBOL(name) name\n'
    '#else\n.section .rodata,"a",@progbits\n#define BLOB_SYMBOL(name) name\n#endif'
)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    gen, old_root = sys.argv[1], sys.argv[2].replace('\\', '/').rstrip('/')
    new_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..')).replace('\\', '/')
    changed = 0
    for dirpath, _, files in os.walk(gen):
        for name in files:
            if not name.endswith(('.cmake', '.S', '.cpp', '.h', '.inc', '.yml', '.txt')):
                continue
            path = os.path.join(dirpath, name)
            with open(path, 'rb') as fh:
                data = fh.read()
            if old_root.encode() not in data and not name.endswith('.S'):
                continue
            text = data.decode('utf-8', 'surrogateescape').replace('\r\n', '\n').replace(old_root, new_root)
            if name.endswith('.S') and 'BLOB_SYMBOL' not in text:
                text = text.replace('.section .rdata,"dr"', PORTABLE_SECTION, 1)
                text = text.replace('.section .rodata,"a",@progbits', PORTABLE_SECTION, 1)
                text = re.sub(r'^\.globl (\w+)$', r'.globl BLOB_SYMBOL(\1)', text, flags=re.M)
                text = re.sub(r'^(kData\w*):$', r'BLOB_SYMBOL(\1):', text, flags=re.M)
                text = text.replace('.section .note.GNU-stack,"",@progbits',
                                    '#if !defined(__APPLE__) && !defined(_WIN32)\n'
                                    '.section .note.GNU-stack,"",@progbits\n#endif')
            with open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='') as fh:
                fh.write(text)
            changed += 1
    print('rewrote %d files under %s' % (changed, gen))


if __name__ == '__main__':
    main()
