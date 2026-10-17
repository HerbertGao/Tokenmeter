import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('flash', ROOT / 'scripts/flash.py')
assert spec is not None and spec.loader is not None
flash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flash)
PORT = {'port': '/dev/cu.usbmodemTEST', 'description': 'ESP32 USB', 'hwid': 'USB VID:PID=303A:1001 SER=DEMO'}


class FlashTests(unittest.TestCase):
    def invoke(self, ports, answers=(), argv=(), tty=True, failure=None):
        commands = []
        def run(command, **kwargs):
            commands.append(command)
            if 'device' in command:
                return subprocess.CompletedProcess(command, 0, json.dumps(ports), '')
            if failure:
                raise subprocess.CalledProcessError(failure, command)
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(flash.subprocess, 'run', side_effect=run), \
             mock.patch.object(flash.sys.stdin, 'isatty', return_value=tty), \
             mock.patch('builtins.input', side_effect=answers), \
             mock.patch.object(flash, 'ROOT', Path('/tmp/project with spaces')), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = flash.main(list(argv))
        return code, commands

    def test_unique_device_requires_confirmation(self):
        code, commands = self.invoke([PORT], ['FLASH'])
        self.assertEqual(code, 0)
        self.assertEqual(commands[-1], [sys.executable, '-m', 'platformio', 'run', '-d', '/tmp/project with spaces/firmware', '-e', 'waveshare_amoled_216', '-t', 'upload', '--upload-port', PORT['port']])
        code, commands = self.invoke([PORT], [''])
        self.assertEqual(code, 1)
        self.assertEqual(len(commands), 1)

    def test_multiple_devices_never_default_to_first(self):
        second = {**PORT, 'port': '/dev/cu.usbmodemSECOND'}
        code, commands = self.invoke([PORT, second], ['2', 'FLASH'])
        self.assertEqual(code, 0)
        self.assertEqual(commands[-1][-1], second['port'])
        for answer in ['', '0', '3', 'not a number']:
            code, commands = self.invoke([PORT, second], [answer])
            self.assertEqual(code, 2)
            self.assertEqual(len(commands), 1)

    def test_no_device_and_non_esp_serial_are_not_auto_selected(self):
        other = {'port': '/dev/cu.other', 'hwid': 'USB VID:PID=0403:6001 SER=303A:1001'}
        for ports in [[], [other], [{'port': '/dev/cu.unknown', 'hwid': None}], [{'port': '/dev/cu.bad', 'hwid': 42}]]:
            code, commands = self.invoke(ports)
            self.assertEqual(code, 2)
            self.assertEqual(len(commands), 1)

    def test_bad_records_do_not_hide_a_valid_port(self):
        for data in [None, {}, [None, {}, 'bad record']]:
            code, commands = self.invoke(data)
            self.assertEqual(code, 2)
            self.assertEqual(len(commands), 1)
        code, commands = self.invoke([None, {}, PORT], ['FLASH'])
        self.assertEqual(code, 0)
        self.assertEqual(commands[-1][-1], PORT['port'])

    def test_explicit_port_must_exist(self):
        for port in ['', '/dev/cu.missing']:
            code, commands = self.invoke([PORT], argv=[port])
            self.assertEqual(code, 2)
            self.assertEqual(len(commands), 1)
        code, commands = self.invoke([PORT], ['FLASH'], [PORT['port']])
        self.assertEqual(code, 0)
        self.assertEqual(commands[-1][-1], PORT['port'])

    def test_cancel_interrupt_and_noninteractive_never_upload(self):
        for interruption in [EOFError(), KeyboardInterrupt()]:
            code, commands = self.invoke([PORT], [interruption])
            self.assertEqual(code, 130)
            self.assertEqual(len(commands), 1)
        code, commands = self.invoke([PORT], tty=False)
        self.assertEqual(code, 2)
        self.assertEqual(commands, [])

    def test_upload_failure_is_not_success(self):
        code, commands = self.invoke([PORT], ['FLASH'], failure=7)
        self.assertEqual(code, 7)
        self.assertEqual(len(commands), 2)


FAKE = '''import json, os, pathlib, shutil, sys
args=sys.argv[1:]
if args[:1]==['-c']: sys.exit(0)
with open(os.environ['TEST_LOG'],'a') as f:
    f.write(json.dumps({'exe':os.environ['TEST_EXE'],'args':args,'cwd':os.getcwd()})+'\\n')
if args[:2]==['-m','venv']:
    dest=pathlib.Path(args[2])/'bin/python';dest.parent.mkdir(parents=True)
    shutil.copy2(os.environ['TEST_BASE'],dest)
if args[:2]==['-m','daemon.tokenmeter']:sys.exit(17)
'''


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='tokenmeter-launchers-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'project with spaces'
        (self.root / 'scripts').mkdir(parents=True)
        for name in ['start-daemon.sh', 'flash.sh', 'scripts/common.sh']:
            shutil.copy2(ROOT / name, self.root / name)
        helper = self.base / 'fake.py'
        helper.write_text(FAKE)
        binary = self.base / 'bin/python3.14'
        binary.parent.mkdir()
        binary.write_text('#!/bin/sh\nexport TEST_EXE="$0"\nexec ' + shlex.quote(sys.executable) + ' ' + shlex.quote(str(helper)) + ' "$@"\n')
        binary.chmod(0o755)
        self.log = self.base / 'calls.jsonl'
        self.env = {**os.environ, 'PATH':str(binary.parent)+':/usr/bin:/bin', 'TEST_LOG':str(self.log), 'TEST_BASE':str(binary)}

    def launch(self, name='start-daemon.sh', args=()):
        return subprocess.run(['/bin/bash', str(self.root / name), *args], cwd=self.base, env=self.env, capture_output=True, text=True)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_fresh_setup_uses_project_venv_and_preserves_arguments(self):
        args = ['--pi-dir', '/tmp/agent with spaces', '--interval', '60']
        result = self.launch(args=args)
        self.assertEqual(result.returncode, 17, result.stderr)
        calls = self.calls()
        self.assertEqual(calls[0]['args'], ['-m', 'venv', str(self.root / '.venv')])
        self.assertEqual(calls[1]['exe'], str(self.root / '.venv/bin/python'))
        self.assertEqual(calls[1]['args'][:4], ['-m', 'pip', '--isolated', 'install'])
        self.assertEqual(calls[1]['args'][-1], str(self.root / 'daemon/requirements.txt'))
        self.assertEqual(calls[-1]['args'], ['-m', 'daemon.tokenmeter', *args])
        self.assertEqual(Path(calls[-1]['cwd']).resolve(), self.root.resolve())

    def test_healthy_venv_is_not_recreated(self):
        binary = self.root / '.venv/bin/python'
        binary.parent.mkdir(parents=True)
        shutil.copy2(self.env['TEST_BASE'], binary)
        result = self.launch()
        self.assertEqual(result.returncode, 17)
        self.assertEqual(len(self.calls()), 2)
        self.assertTrue(all(c['exe']==str(binary) for c in self.calls()))

    def test_once_and_help_do_not_install_or_create_environment(self):
        for option in ['--once', '--help']:
            self.log.unlink(missing_ok=True)
            result = self.launch(args=[option])
            self.assertEqual(result.returncode, 17)
            self.assertEqual([c['args'] for c in self.calls()], [['-m','daemon.tokenmeter',option]])
            self.assertFalse((self.root / '.venv').exists())

    def test_broken_environment_is_not_deleted(self):
        keep = self.root / '.venv/keep.txt'
        keep.parent.mkdir()
        keep.write_text('preserve user files')
        result = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(keep.read_text(), 'preserve user files')
        self.assertEqual(self.calls(), [])

    def test_global_python_is_not_accepted_as_a_virtualenv(self):
        binary = self.root / '.venv/bin/python'
        binary.parent.mkdir(parents=True)
        binary.symlink_to(getattr(sys, '_base_executable', sys.executable))
        result = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(binary.is_symlink())
        self.assertEqual(self.calls(), [])

    def test_flash_help_and_noninteractive_have_no_setup_side_effects(self):
        result = self.launch('flash.sh', ['--help'])
        self.assertEqual(result.returncode, 0)
        result = self.launch('flash.sh')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls(), [])
        self.assertFalse((self.root / '.venv').exists())


if __name__ == '__main__':
    unittest.main()
