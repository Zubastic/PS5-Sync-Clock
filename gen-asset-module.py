#!/usr/bin/env python3
# Embed a static asset into a C module that self-registers at startup.
# Based on gen-asset-module.py from ps5-payload-sdk (John Tornblom),
# with --path/--mime overrides.

import argparse
import mimetypes
import string


tmpl = string.Template('''
// Generated from $file by gen-asset-module.py. Do not edit.
// Compiled as C++ so asset_register resolves with C++ linkage.

void asset_register(const char*, void*, unsigned long, const char*);

static unsigned char data[] = $data;

__attribute__((constructor)) static void
constructor(void) {
  asset_register("$path", data, sizeof(data), "$mime");
}
''')


def gen_data(filename):
    yield '{\n  '

    with open(filename, mode='rb') as f:
        for n, b in enumerate(f.read(), 1):
            yield hex(b)
            yield ', '

            if n % 16 == 0:
                yield '\n  '

    yield '\n}'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', '--path', default=None,
                        help='URL path to register (default: file name)')
    parser.add_argument('-m', '--mime', default=None,
                        help='MIME type (default: guessed from the name)')
    parser.add_argument('FILE')
    args = parser.parse_args()

    path = args.path if args.path is not None else args.FILE
    path = '/' + path.lstrip('/')
    mime = args.mime or mimetypes.guess_type(args.FILE)[0] or 'application/octet-stream'

    data = ''.join(gen_data(args.FILE))
    print(tmpl.substitute(data=data, path=path, mime=mime, file=args.FILE))
