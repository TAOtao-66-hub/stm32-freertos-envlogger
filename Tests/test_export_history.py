"""PC protocol/file checks; no board or pyserial installation is required."""
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('export_history', ROOT/'Tools/export_history.py')
export = importlib.util.module_from_spec(spec)
spec.loader.exec_module(export)
HEADER = ','.join(export.COLUMNS)
ROW = '3,1,30,2026 10 19 21:34:01,25.5,58.0,1,0,rtc'
DATA = ('ENV TEMP=25.5C\r\nCSV_BEGIN snapshot=3 order=NEWEST_FIRST\r\n' + HEADER + '\r\n' +
        ROW + '\r\n2,1,20,UNCALIBRATED,25.4,59.0,0,4,unknown\r\n' +
        'CSV_END rows=2 skipped=1 state=OK\r\nENV TEMP=25.6C\r\n').encode('ascii')


class Port:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.writes = []

    def read(self, count):
        if self.chunks:
            chunk = self.chunks.pop(0)
            if isinstance(chunk, BaseException):
                raise chunk
            return chunk
        raise TimeoutError('test port exhausted before CSV_END')

    def write(self, data):
        self.writes.append(data)
        return len(data)


class ExportTests(unittest.TestCase):
    def receiver(self):
        result = export.CsvReceiver(io.StringIO())
        result.feed('CSV_BEGIN snapshot=3 order=NEWEST_FIRST')
        result.feed(HEADER)
        return result

    def test_every_usb_split_preserves_rows_and_spaces(self):
        for split in range(1, len(DATA)):
            with self.subTest(split=split):
                stream = io.StringIO()
                result = export.receive_export(Port([DATA[:split], DATA[split:]]), stream)
                self.assertEqual((result.rows, result.skipped), (2, 1))
                self.assertIn('2026 10 19 21:34:01', stream.getvalue())
                self.assertNotIn('CSV_END', stream.getvalue())
                self.assertNotIn('ENV', stream.getvalue())

    def test_single_byte_usb_chunks(self):
        result = export.receive_export(Port([bytes([x]) for x in DATA]), io.StringIO())
        self.assertTrue(result.done)

    def test_empty_history_is_valid(self):
        result = self.receiver()
        result.feed('CSV_END rows=0 skipped=0 state=OK')
        self.assertTrue(result.done)

    def test_all_failure_endings_are_incomplete(self):
        for state in ('CANCELLED', 'NOT_READY', 'READ_IO', 'OVERRUN'):
            with self.subTest(state=state), self.assertRaises(export.ProtocolError):
                self.receiver().feed('CSV_END rows=0 skipped=0 state=' + state)

    def test_row_count_mismatch(self):
        result = self.receiver()
        result.feed(ROW)
        with self.assertRaises(export.ProtocolError):
            result.feed('CSV_END rows=2 skipped=0 state=OK')

    def test_duplicate_out_of_snapshot_and_ascending_sequence(self):
        for row in (ROW, ROW.replace('3,1,', '4,1,'), ROW.replace('3,1,', '0,1,')):
            result = self.receiver()
            result.feed(ROW)
            with self.assertRaises(export.ProtocolError):
                result.feed(row)

    def test_malformed_fields_and_interleaved_text(self):
        for row in ('ENV TEMP=25.5C', ROW.replace(',1,0,rtc', ',2,0,rtc'),
                    ROW.replace('2026 10 19', '2026-10-19'),
                    ROW.replace('2026 10 19', '2026 02 30'),
                    ROW.replace('rtc', 'unknown'), ROW.replace('25.5', 'nan'),
                    ROW.replace(',1,0,rtc', ',1,8,rtc')):
            with self.subTest(row=row), self.assertRaises(export.ProtocolError):
                self.receiver().feed(row)

    def test_missing_header(self):
        receiver = export.CsvReceiver(io.StringIO())
        receiver.feed('CSV_BEGIN snapshot=3 order=NEWEST_FIRST')
        with self.assertRaises(export.ProtocolError):
            receiver.feed(ROW)

    def test_error_before_begin(self):
        with self.assertRaises(export.ProtocolError):
            export.CsvReceiver(io.StringIO()).feed('ERR STORAGE_BUSY')

    def test_success_creates_final_csv(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)/'history.csv'
            port = Port([DATA])
            result = export.export_to_file(port, target)
            self.assertEqual(result.rows, 2)
            self.assertEqual(port.writes, [b'log export\r\n'])
            self.assertIn(ROW, target.read_text(encoding='utf-8-sig'))
            self.assertFalse(target.with_name('history.csv.part').exists())

    def test_failure_retains_partial_and_cancels(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)/'history.csv'
            port = Port([DATA.replace(b'state=OK', b'state=OVERRUN')])
            with self.assertRaises(export.ProtocolError):
                export.export_to_file(port, target)
            self.assertFalse(target.exists())
            self.assertTrue(target.with_name('history.csv.part').exists())
            self.assertEqual(port.writes[-1], b'log cancel\r\n')

    def test_existing_output_and_partial_are_never_overwritten(self):
        for filename in ('history.csv', 'history.csv.part'):
            with tempfile.TemporaryDirectory() as folder:
                existing = Path(folder)/filename
                existing.write_text('keep', encoding='utf-8')
                port = Port([DATA])
                with self.assertRaises(FileExistsError):
                    export.export_to_file(port, Path(folder)/'history.csv')
                self.assertEqual(existing.read_text(encoding='utf-8'), 'keep')
                self.assertEqual(port.writes, [])

    def test_disconnect_and_ctrl_c_preserve_partial(self):
        for error in (OSError('disconnected'), KeyboardInterrupt()):
            with tempfile.TemporaryDirectory() as folder:
                target = Path(folder)/'history.csv'
                port = Port([error])
                with self.assertRaises(type(error)):
                    export.export_to_file(port, target)
                self.assertFalse(target.exists())
                self.assertTrue(target.with_name('history.csv.part').exists())
                self.assertEqual(port.writes[-1], b'log cancel\r\n')

    def test_missing_newline_and_non_ascii_are_rejected(self):
        for chunks in ([b'x'*256, b'x'*10], [b'\xff\n']):
            with self.assertRaises(export.ProtocolError):
                export.receive_export(Port(chunks), io.StringIO())

    def test_idle_timeout_rejects_missing_completion(self):
        with patch.object(export.time, 'monotonic', side_effect=[0.0, 61.0]):
            with self.assertRaises(TimeoutError):
                export.receive_export(Port([b'']), io.StringIO(), timeout=60.0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
