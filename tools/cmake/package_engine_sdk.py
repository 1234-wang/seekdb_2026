#!/usr/bin/env python3
"""Bundle the native link closure into a relocatable SDK directory (GNU/LLVM ar)."""
import argparse
import pathlib
import shlex
import shutil
import subprocess
import tempfile


def collect(command, directory):
    args = shlex.split(command)
    expanded = []
    for arg in args:
        if arg.startswith('@'):
            expanded.extend(shlex.split((directory / arg[1:]).read_text()))
        else:
            expanded.append(arg)
    search = [(directory / arg[2:]).resolve() for arg in expanded if arg.startswith('-L')]
    inputs, system = [], []
    for arg in expanded:
        path = None
        if arg.endswith(('.a', '.o')):
            path = (directory / arg).resolve()
            if 'ob_main.dir' in arg or path.name == 'libmalloc_hook.a':
                continue
        elif arg.startswith('-l'):
            name = arg[2:]
            path = next((root / ('lib' + name + '.a') for root in search
                         if (root / ('lib' + name + '.a')).is_file()), None)
            if path is None:
                if name not in {'pthread', 'dl', 'm', 'rt', 'aio', 'c', 'stdc++'}:
                    raise ValueError('Unbundled non-system dependency: ' + arg)
                if name not in system:
                    system.append(name)
        if path is not None and path not in inputs:
            if not path.is_file():
                raise FileNotFoundError(path)
            inputs.append(path)
    if not inputs:
        raise ValueError('Empty engine link closure')
    return inputs, system


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--link-command', type=pathlib.Path, required=True)
    parser.add_argument('--ar', required=True)
    parser.add_argument('--header', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    inputs, system = collect(args.link_command.read_text(), args.link_command.parent.parent.parent)
    root = args.output.resolve()
    lib = root / 'lib'
    include = root / 'include'
    config = lib / 'cmake' / 'SeekDB'
    for directory in (lib, include, config):
        directory.mkdir(parents=True, exist_ok=True)
    output = lib / 'liboceanbase_static.a'
    # MRI scripts do not portably quote whitespace. Use relative aliases in a
    # temporary directory so paths with spaces work with both GNU and LLVM ar.
    with tempfile.TemporaryDirectory(prefix='engine-archive-', dir=lib) as scratch:
        scratch = pathlib.Path(scratch)
        lines = ['CREATE engine.a']
        for index, path in enumerate(inputs):
            alias = 'input' + str(index) + path.suffix
            (scratch / alias).symlink_to(path)
            lines.append(('ADDLIB ' if path.suffix == '.a' else 'ADDMOD ') + alias)
        lines.extend(['SAVE', 'END'])
        subprocess.run([args.ar, '-M'], cwd=scratch,
                       input='\n'.join(lines) + '\n', text=True, check=True)
        (scratch / 'engine.a').replace(output)
    shutil.copy2(args.header, include / 'seekdb_engine.h')
    (config / 'SeekDBConfig.cmake').write_text('''get_filename_component(_seekdb_sdk "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET SeekDB::Engine)
  add_library(SeekDB::Engine STATIC IMPORTED)
  set_target_properties(SeekDB::Engine PROPERTIES
    IMPORTED_LOCATION "${_seekdb_sdk}/lib/liboceanbase_static.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_seekdb_sdk}/include"
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
    INTERFACE_LINK_LIBRARIES "''' + ';'.join(system) + '''")
endif()
unset(_seekdb_sdk)
''')


if __name__ == '__main__':
    main()
