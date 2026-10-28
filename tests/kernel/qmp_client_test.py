"""Shared QMP transport on a fake Unix peer, without a guest or host UI."""
from contextlib import contextmanager
import importlib.util
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('liteos_qmp', ROOT / 'tools/qmp_client.py')
qmp_module = importlib.util.module_from_spec(spec)
with mock.patch('socket.socket', side_effect=AssertionError('import must not connect')):
    spec.loader.exec_module(qmp_module)
QMPClient = qmp_module.QMPClient


@contextmanager
def peer(respond, greeting=None):
    with tempfile.TemporaryDirectory(prefix='liteos-qmp-unit-') as directory:
        path = Path(directory) / 'qmp.sock'
        listener = socket.socket(socket.AF_UNIX)
        listener.bind(str(path))
        listener.listen(1)
        listener.settimeout(2)
        failures = []
        received = []
        def serve():
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(2)
                    with connection.makefile('rwb') as stream:
                        first = greeting if greeting is not None else {'QMP': {'version': {}}}
                        stream.write((json.dumps(first) + '\n').encode())
                        stream.flush()
                        for line in stream:
                            request = json.loads(line)
                            received.append(request)
                            if request['execute'] == 'qmp_capabilities':
                                replies = [{'return': {}, 'id': request['id']}]
                            else:
                                replies = respond(request)
                            if replies is None:
                                return
                            for reply in replies:
                                stream.write(reply if isinstance(reply, bytes) else
                                             (json.dumps(reply) + '\n').encode())
                            stream.flush()
            except BaseException as error:
                failures.append(error)
            finally:
                listener.close()
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            yield path, received
        finally:
            thread.join(3)
            listener.close()
            assert not thread.is_alive(), 'QMP peer leaked a connection'
            assert not failures, failures


class QMPTests(unittest.TestCase):
    def test_default_endpoint_uses_canonical_build(self):
        def respond(request):
            return [{'return': {'running': True}, 'id': request['id']}]
        with peer(respond) as (path, requests):
            build = path.parent / 'build'
            build.mkdir()
            (build / 'qemu-qmp.sock').symlink_to(path)
            with mock.patch.object(qmp_module, 'ROOT', path.parent):
                with QMPClient() as client:
                    self.assertEqual(client.command('query-status'), {'running': True})
            self.assertEqual([r['execute'] for r in requests],
                             ['qmp_capabilities', 'query-status'])

    def test_handshake_events_ids_and_command_error_recovery(self):
        def respond(request):
            result = {'running': True} if request['execute'] == 'query-status' else None
            reply = {'return': result, 'id': request['id']}
            if request['execute'] == 'bad-command':
                reply = {'error': {'class': 'CommandNotFound', 'desc': 'not found'},
                         'id': request['id']}
            return [{'event': 'RESET'}, reply]
        with peer(respond) as (path, requests):
            with QMPClient(path) as client:
                self.assertEqual(client.command('query-status'), {'running': True})
                with self.assertRaises(qmp_module.QMPCommandError) as error:
                    client.command('bad-command')
                self.assertEqual(error.exception.args[0]['error']['class'], 'CommandNotFound')
                self.assertIsNone(client.command('null-result', {'a': 1}))
                self.assertEqual(client.sock.gettimeout(), 10)
            self.assertEqual(client.sock.fileno(), -1)
            self.assertTrue(client._closed)
            client.close()
            with self.assertRaisesRegex(RuntimeError, 'closed'):
                client.command('query-status')
            self.assertEqual([r['id'] for r in requests], [1, 2, 3, 4])
            self.assertEqual(requests[0]['execute'], 'qmp_capabilities')
            self.assertEqual(requests[-1]['arguments'], {'a': 1})

    def test_protocol_failures_close_resources(self):
        replies = [None, [b'not-json\n'], [b'[]\n'], [{'return': {}, 'id': 99}],
                   [{'id': 2}]]
        for response in replies:
            with self.subTest(response=response), peer(lambda r: response) as (path, _):
                client = QMPClient(path)
                with self.assertRaises((EOFError, ValueError)):
                    client.command('query-status')
                self.assertTrue(client._closed)
                self.assertEqual(client.sock.fileno(), -1)

    def test_bad_greeting_and_connect_failure_close_socket(self):
        real_socket = socket.socket
        created = []
        def record(*args, **kwargs):
            value = real_socket(*args, **kwargs)
            created.append(value)
            return value
        with peer(lambda r: [], greeting={'event': 'RESET'}) as (path, _):
            with mock.patch.object(qmp_module.socket, 'socket', side_effect=record):
                with self.assertRaisesRegex(RuntimeError, 'greeting'):
                    QMPClient(path)
            self.assertEqual(created[0].fileno(), -1)
        with tempfile.TemporaryDirectory(prefix='liteos-qmp-missing-') as directory:
            with mock.patch.object(qmp_module.socket, 'socket', side_effect=record):
                with self.assertRaises(FileNotFoundError):
                    QMPClient(Path(directory) / 'absent.sock')
            self.assertEqual(created[-1].fileno(), -1)
        with mock.patch.object(qmp_module.socket, 'socket', side_effect=record):
            with self.assertRaises(ValueError):
                QMPClient(timeout=-1)
        self.assertEqual(created[-1].fileno(), -1)

    def test_input_helpers_preserve_guest_events_and_timing(self):
        client = object.__new__(QMPClient)
        calls = []
        client.command = lambda name, args=None: calls.append((name, args))
        with mock.patch.object(qmp_module.time, 'sleep') as sleep:
            client.move(125, 50)
            self.assertEqual(sum(c[1]['events'][0]['data']['value'] for c in calls), -2375)
            self.assertEqual(sum(c[1]['events'][1]['data']['value'] for c in calls), -1450)
            self.assertTrue(all(abs(e['data']['value']) <= 80
                                for c in calls for e in c[1]['events']))
            self.assertEqual(sleep.call_args_list,
                             [mock.call(.15)] * 32 + [mock.call(.3)] +
                             [mock.call(.15)] * 2 + [mock.call(.3)])
            calls.clear(); sleep.reset_mock()
            client.move = mock.Mock()
            client.click(10, 20, 'right')
            client.move.assert_called_once_with(10, 20)
            self.assertEqual([c[1]['events'][0]['data'] for c in calls],
                             [{'button': 'right', 'down': True},
                              {'button': 'right', 'down': False}])
            self.assertEqual(sleep.call_args_list, [mock.call(.04), mock.call(.04), mock.call(.2)])
            calls.clear(); sleep.reset_mock()
            client.key('ctrl', 'equal')
            self.assertEqual([e['data']['key']['data'] for c in calls for e in c[1]['events']],
                             ['ctrl', 'equal', 'equal', 'ctrl'])
            self.assertEqual(sleep.call_args_list, [mock.call(.04), mock.call(.04)])
            calls.clear()
            client.screenshot(ROOT / 'build/test.png')
            self.assertEqual(calls, [('screendump', {
                'filename': str(ROOT / 'build/test.png'), 'format': 'png'})])

    def test_motion_pacing_with_bounded_coalescing_hid_reader(self):
        # Conservative signed-byte reader: movements queued before a sample
        # coalesce and clamp. A host enqueue ACK is not that sample.
        for start in [(0, 0), (1919, 1079), (960, 540)]:
            for target in [(0, 0), (815, 1020), (935, 30), (1666, 155)]:
                position = list(start)
                pending = [0, 0]
                elapsed = 0.0
                next_sample = .1
                client = object.__new__(QMPClient)

                def send(name, args):
                    self.assertEqual(name, 'input-send-event')
                    for event in args['events']:
                        data = event['data']
                        pending[data['axis'] == 'y'] += data['value']

                def advance(seconds):
                    nonlocal elapsed, next_sample
                    elapsed += seconds
                    while next_sample <= elapsed:
                        for axis, maximum in [(0, 1919), (1, 1079)]:
                            delta = max(-127, min(127, pending[axis]))
                            position[axis] = max(0, min(maximum, position[axis] + delta))
                            pending[axis] = 0
                        next_sample += .1

                client.command = send
                with mock.patch.object(qmp_module.time, 'sleep', side_effect=advance):
                    client.move(*target)
                self.assertEqual(position, list(target), (start, target))
                self.assertEqual(pending, [0, 0])

    def test_timeout_closes_connection_without_restart(self):
        def delay(request):
            threading.Event().wait(.08)
            return None
        with peer(delay) as (path, _):
            client = QMPClient(path)
            client.sock.settimeout(.02)
            with self.assertRaises(TimeoutError):
                client.command('query-status')
            self.assertTrue(client._closed)
            self.assertEqual(client.sock.fileno(), -1)

    def test_canonical_qa_imports_shared_module_without_source_eval(self):
        import ast
        paths = ['tools/chromium/qmp-taskmgr-test.py',
                 'tools/chromium/qmp-gshell-launch-test.py',
                 'tools/chromium/qmp-gshell-viewport-test.py',
                 'tools/nvidia/qmp-cube-drag-test.py',
                 'tools/nvidia/qmp-window-resize-test.py']
        for path in paths:
            code = (ROOT / path).read_text()
            tree = ast.parse(code, filename=path)
            self.assertTrue(any(isinstance(n, ast.ImportFrom) and n.module == 'qmp_client'
                                for n in ast.walk(tree)), path)
            self.assertFalse(any(isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
                                 and n.func.id == 'exec' for n in ast.walk(tree)), path)
            compile(tree, path, 'exec')

    def test_event_flood_cannot_extend_command_deadline(self):
        client = object.__new__(QMPClient)
        client._closed = False
        client._sequence = 0
        client.sock = mock.Mock()
        client.sock.gettimeout.return_value = .02
        client._buffer = bytearray()
        client._read = mock.Mock(return_value={'event': 'RESET'})
        with mock.patch.object(qmp_module.time, 'monotonic',
                               side_effect=[0, .005, .015, .025]):
            with self.assertRaisesRegex(TimeoutError, 'deadline'):
                client.command('query-status')
        self.assertEqual(client._read.call_count, 2)
        remaining = [c.args[0] for c in client.sock.settimeout.call_args_list]
        self.assertAlmostEqual(remaining[0], .015)
        self.assertAlmostEqual(remaining[1], .005)
        self.assertTrue(client._closed)
        client.sock.close.assert_called_once()
        self.assertEqual(client._buffer, bytearray())

    def test_explicit_blocking_mode_preserves_socket_timeout(self):
        client = object.__new__(QMPClient)
        client._closed = False
        client._sequence = 0
        client.sock = mock.Mock()
        client.sock.gettimeout.return_value = None
        client._buffer = bytearray()
        client._read = mock.Mock(side_effect=[{'event': 'RESET'}, {'id': 1, 'return': False}])
        with mock.patch.object(qmp_module.time, 'monotonic') as clock:
            self.assertIs(client.command('query-status'), False)
            clock.assert_not_called()
        client.sock.settimeout.assert_called_once_with(None)
        self.assertFalse(client._closed)
        client.close()

    def test_fragmented_response_cannot_extend_command_deadline(self):
        client = object.__new__(QMPClient)
        client._closed = False
        client._sequence = 0
        client._buffer = bytearray()
        client.sock = mock.Mock()
        client.sock.gettimeout.return_value = .02
        client.sock.recv.side_effect = [b'{"ret', b'urn":', b' true', b'}\n']
        with mock.patch.object(qmp_module.time, 'monotonic',
                               side_effect=[0, .001, .005, .01, .015, .025]):
            with self.assertRaisesRegex(TimeoutError, 'deadline'):
                client.command('query-status')
        self.assertEqual(client.sock.recv.call_count, 3)
        self.assertTrue(client._closed)
        self.assertEqual(client._buffer, bytearray())
        client.sock.close.assert_called_once()

    def test_multiple_and_fragmented_messages_preserve_framing(self):
        client = object.__new__(QMPClient)
        client._closed = False
        client._sequence = 0
        client._buffer = bytearray()
        client.sock = mock.Mock()
        client.sock.gettimeout.return_value = None
        client.sock.recv.side_effect = [
            b'{"event": "RESET"}\n{"id": 1, "ret',
            b'urn": true}\n{"id": 2, "return": false}\n']
        self.assertIs(client.command('query-status'), True)
        self.assertIs(client.command('query-status'), False)
        self.assertEqual(client.sock.recv.call_count, 2)
        self.assertEqual(client._buffer, bytearray())
        client.close()

    def test_all_legacy_helpers_delegate_and_short_connections_close(self):
        import ast
        from types import SimpleNamespace
        matched = 0
        for path in (ROOT / 'tools/chromium').glob('*.py'):
            tree = ast.parse(path.read_text(), filename=str(path))
            functions = [n for n in tree.body if isinstance(n, ast.FunctionDef)
                         and n.name in ('command', 'command_once', 'commands')]
            if not functions:
                continue
            matched += 1
            self.assertTrue(any(isinstance(n, ast.ImportFrom) and n.module == 'qmp_client'
                                for n in tree.body), path.name)
            spy = mock.MagicMock()
            spy.__enter__.return_value = spy
            token = object()
            spy.command.return_value = token
            namespace = {'qmp': spy, 'QMPClient': lambda: spy,
                         'time': SimpleNamespace(monotonic=lambda: 0, sleep=lambda n: None)}
            exec(compile(ast.Module(body=functions, type_ignores=[]), str(path), 'exec'), namespace)
            if 'commands' in namespace:
                self.assertEqual(namespace['commands']([('query-status', {'x': 1}),
                                                       ('query-name', {'x': 2})]), [token, token])
                self.assertEqual(spy.command.call_args_list, [mock.call('query-status', {'x': 1}),
                                                             mock.call('query-name', {'x': 2})])
                spy.__exit__.assert_called_once()
            else:
                self.assertIs(namespace['command']('query-status', {'x': 1}), token)
                spy.command.assert_called_once_with('query-status', {'x': 1})
                if path.name == 'qmp-trace-lifecycle.py':
                    spy.__exit__.assert_called_once()
        self.assertEqual(matched, 27)

    def test_debugger_retry_is_only_for_rejected_input(self):
        import ast
        from types import SimpleNamespace
        for filename, delay in [('qmp-address-latency.py', .1), ('qmp-trace-keys.py', .02)]:
            tree = ast.parse((ROOT / 'tools/chromium' / filename).read_text())
            functions = [n for n in tree.body if isinstance(n, ast.FunctionDef)
                         and n.name in ('command', 'command_once')]
            spy = mock.Mock()
            sleep = mock.Mock()
            namespace = {'qmp': spy, 'time': SimpleNamespace(monotonic=lambda: 0, sleep=sleep)}
            exec(compile(ast.Module(body=functions, type_ignores=[]), filename, 'exec'), namespace)
            rejected = qmp_module.QMPCommandError({'error': {'desc': 'VM not running'}})
            spy.command.side_effect = [rejected, 'delivered']
            with mock.patch('builtins.print'):
                self.assertEqual(namespace['command']('input-send-event', {'events': []}), 'delivered')
            self.assertEqual(spy.command.call_count, 2)
            sleep.assert_called_once_with(delay)
            spy.command.reset_mock(); sleep.reset_mock()
            spy.command.side_effect = [rejected, 'should not retry']
            with self.assertRaises(qmp_module.QMPCommandError):
                namespace['command']('query-status')
            self.assertEqual(spy.command.call_count, 1)
            sleep.assert_not_called()


if __name__ == '__main__':
    unittest.main()
