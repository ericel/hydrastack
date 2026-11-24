"""Process-level regressions for local builds; no compiler or browser needed."""

import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


SUPERVISOR = Path(__file__).resolve().parents[1] / 'hydrastack/scaffold/scripts/dev_supervisor.py'
RUN_ONCE = SUPERVISOR.with_name('run_drogon_dev_once.sh')


class DevSupervisorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='hydra dev ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.build = self.root / 'build'
        self.build.mkdir()
        self.source = self.root / 'main.cc'
        self.source.write_text('initial')
        self.config = self.root / 'config.json'
        self.config.write_text('{}')
        self.assets = self.root / 'assets'
        self.assets.mkdir()
        self.events = self.root / 'events'
        self.release = self.root / 'release'
        self.failure = self.root / 'fail'
        self.processes = []
        self.logs = []
        self.addCleanup(self.cleanup)
        self.env = {**os.environ, 'HYDRA_PROJECT_ROOT': str(self.root),
                    'HYDRA_BUILD_DIR': str(self.build),
                    'HYDRA_CONFIG_PATH': str(self.config),
                    'HYDRA_DEV_SUPERVISOR': str(SUPERVISOR),
                    'TEST_EVENTS': str(self.events),
                    'TEST_RELEASE': str(self.release),
                    'TEST_FAIL': str(self.failure)}
        self.env.pop('CMAKE_BUILD_PARALLEL_LEVEL', None)
        self.env.pop('HYDRA_APP_TARGET', None)
        self.fake = self.root / 'fake.py'
        self.fake.write_text('''import os, pathlib, signal, subprocess, sys, time
events = pathlib.Path(os.environ['TEST_EVENTS'])
def event(name):
    with events.open('a') as out:
        out.write(f'{name} {os.getpid()}\\n')
mode = sys.argv[1]
if mode == 'cmake':
    event('build-start')
    with events.open('a') as out:
        out.write('arguments ' + ' '.join(sys.argv[2:]) + '\\n')
    while not pathlib.Path(os.environ['TEST_RELEASE']).exists():
        time.sleep(.03)
    event('build-end')
    sys.exit(1 if pathlib.Path(os.environ['TEST_FAIL']).exists() else 0)
if mode == 'server':
    event('server-start')
    subprocess.Popen([sys.executable, __file__, 'child'])
elif mode == 'npm':
    event('npm-start')
    subprocess.Popen([sys.executable, __file__, 'ui-child'])
elif mode == 'ui-child':
    event('ui-child-start')
elif mode == 'child':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    event('child-start')
while True:
    time.sleep(.1)
''')
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.stub(self.bin / 'cmake', 'cmake')
        self.stub(self.build / 'hydra_app', 'server')
        (self.build / 'CMakeCache.txt').touch()
        self.env['PATH'] = str(self.bin) + os.pathsep + self.env['PATH']

    def stub(self, path, mode):
        import shlex
        path.write_text('#!/bin/bash\nexec ' + shlex.join([sys.executable, str(self.fake), mode]) + ' "$@"\n')
        path.chmod(0o755)

    def cleanup(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
        for log in self.logs:
            log.close()

    def start(self, command):
        log = (self.root / f'log-{len(self.logs)}').open('w+')
        self.logs.append(log)
        process = subprocess.Popen(command, env=self.env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        self.processes.append(process)
        return process

    def watch(self):
        return self.start([sys.executable, str(SUPERVISOR), 'watch',
                           '--root', str(self.root), '--build-dir', str(self.build),
                           '--config', str(self.config), '--run-script', str(RUN_ONCE),
                           '--source', str(self.source), '--runtime', str(self.assets),
                           '--runtime', str(self.config)])

    def lines(self, prefix):
        text = self.events.read_text() if self.events.exists() else ''
        return [line for line in text.splitlines() if line.startswith(prefix + ' ')]

    def until(self, condition, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(.05)
        self.fail('Timed out; events: ' + (self.events.read_text() if self.events.exists() else 'none'))

    def test_source_edits_queue_one_build_and_assets_only_restart(self):
        process = self.watch()
        self.until(lambda: len(self.lines('build-start')) == 1)
        self.source.write_text('changed during compilation')
        (self.assets / 'ssr.js').write_text('new bundle')
        time.sleep(.7)
        self.assertEqual(len(self.lines('build-start')), 1)
        self.release.touch()
        self.until(lambda: len(self.lines('child-start')) == 1)
        self.assertEqual(len(self.lines('build-start')), 2)
        self.assertEqual([line.split()[0] for line in self.events.read_text().splitlines()
                          if line.startswith('build-')],
                         ['build-start', 'build-end', 'build-start', 'build-end'])
        (self.assets / 'ssr.js').write_text('asset-only edit')
        self.until(lambda: len(self.lines('child-start')) == 2)
        self.assertEqual(len(self.lines('build-start')), 2)
        self.config.write_text('{"changed":true}')
        self.until(lambda: len(self.lines('child-start')) == 3)
        self.assertEqual(len(self.lines('build-start')), 2)
        process.terminate()
        self.assertEqual(process.wait(timeout=12), 0, '\n'.join(Path(log.name).read_text() for log in self.logs))
        for line in self.lines('child-start'):
            pid = int(line.split()[1])
            self.until(lambda: not self.alive(pid))

    @staticmethod
    def alive(pid):
        result = subprocess.run(['ps', '-p', str(pid), '-o', 'stat='],
                                capture_output=True, text=True)
        return result.returncode == 0 and not result.stdout.strip().startswith('Z')

    def test_asset_change_during_initial_build_does_not_duplicate_build(self):
        self.watch()
        self.until(lambda: len(self.lines('build-start')) == 1)
        (self.assets / 'ssr.js').write_text('initial SSR output')
        time.sleep(.7)
        self.release.touch()
        self.until(lambda: len(self.lines('server-start')) == 1)
        self.assertEqual(len(self.lines('build-start')), 1)

    def test_direct_builds_share_lock_and_default_to_one_worker(self):
        first = self.start(['bash', str(RUN_ONCE), '--build-only'])
        self.until(lambda: len(self.lines('build-start')) == 1)
        second = self.start(['bash', str(RUN_ONCE), '--build-only'])
        time.sleep(.7)
        self.assertEqual(len(self.lines('build-start')), 1)
        self.release.touch()
        self.assertEqual(first.wait(timeout=10), 0)
        self.assertEqual(second.wait(timeout=10), 0)
        self.assertEqual(len(self.lines('build-start')), 2)
        self.assertTrue(all('--target hydra_app --parallel 1' in line
                            for line in self.lines('arguments')))
        self.assertEqual(self.lines('server-start'), [])

    def test_shutdown_during_build_terminates_compiler_and_releases_lock(self):
        process = self.watch()
        self.until(lambda: len(self.lines('build-start')) == 1)
        compiler = int(self.lines('build-start')[0].split()[1])
        process.terminate()
        self.assertEqual(process.wait(timeout=12), 0, '\n'.join(Path(log.name).read_text() for log in self.logs))
        self.until(lambda: not self.alive(compiler))
        self.release.touch()
        direct = self.start(['bash', str(RUN_ONCE), '--build-only'])
        self.assertEqual(direct.wait(timeout=10), 0)

    def test_second_watcher_fails_without_starting_another_build(self):
        self.watch()
        self.until(lambda: len(self.lines('build-start')) == 1)
        second = self.watch()
        self.assertEqual(second.wait(timeout=10), 1)
        self.assertEqual(len(self.lines('build-start')), 1)

    def test_full_dev_script_cleans_up_ui_and_server_process_groups(self):
        ui = self.root / 'ui'
        (ui / 'node_modules/.bin').mkdir(parents=True)
        (ui / 'package.json').write_text('{}')
        self.stub(ui / 'node_modules/.bin/vite', 'npm')
        self.stub(self.bin / 'npm', 'npm')
        lsof = self.bin / 'lsof'
        lsof.write_text('#!/bin/bash\nexit 0\n')
        lsof.chmod(0o755)
        (self.root / 'app/src').mkdir(parents=True)
        self.release.touch()
        process = self.start(['bash', str(SUPERVISOR.with_name('dev.sh'))])
        self.until(lambda: len(self.lines('child-start')) == 1)
        self.assertEqual(len(self.lines('npm-start')), 2)
        self.until(lambda: len(self.lines('ui-child-start')) == 2)
        process.terminate()
        self.assertEqual(process.wait(timeout=20), 143,
                         '\n'.join(Path(log.name).read_text() for log in self.logs))
        for prefix in ('server-start', 'child-start', 'npm-start', 'ui-child-start'):
            for line in self.lines(prefix):
                pid = int(line.split()[1])
                self.until(lambda: not self.alive(pid))

    def test_build_failure_waits_for_source_edit(self):
        self.failure.touch()
        self.release.touch()
        self.watch()
        self.until(lambda: len(self.lines('build-end')) == 1)
        time.sleep(.7)
        (self.assets / 'ssr.js').write_text('must not launch stale app')
        time.sleep(.7)
        self.assertEqual(len(self.lines('build-start')), 1)
        self.assertEqual(self.lines('server-start'), [])
        self.failure.unlink()
        self.source.write_text('fixed')
        self.until(lambda: len(self.lines('server-start')) == 1)
        self.assertEqual(len(self.lines('build-start')), 2)


if __name__ == '__main__':
    unittest.main()
