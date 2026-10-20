"""Export the STM32 history snapshot over USART1 to a validated CSV file.

Example: python Tools/export_history.py --port COM7 --output history.csv
Install pyserial first; close the serial assistant so the port is available.
The board's local Beijing date format is YYYY MM DD HH:MM:SS.
"""
import argparse
import csv
from datetime import datetime
from pathlib import Path
import re
import sys
import time

COLUMNS = ['sequence', 'session', 'uptime_s', 'beijingtime', 'temp_c',
           'humidity_pct', 'valid', 'alarm', 'time_source']
BEGIN = re.compile(r'CSV_BEGIN snapshot=(\d+) order=NEWEST_FIRST')
END = re.compile(r'CSV_END rows=(\d+) skipped=(\d+) state=([A-Z_]+)')


class ProtocolError(ValueError):
    """The transfer cannot be accepted as a complete export."""


class CsvReceiver:
    """Validate one complete line at a time; USB chunks may split anywhere."""

    def __init__(self, stream):
        self.writer = csv.writer(stream, lineterminator='\n')
        self.started = False
        self.header = False
        self.done = False
        self.snapshot = 0
        self.previous = None
        self.rows = 0
        self.skipped = 0

    def feed(self, line):
        if self.done:
            raise ProtocolError('Data after CSV_END')
        if not self.started:
            match = BEGIN.fullmatch(line)
            if not match:
                if line.startswith('ERR '):
                    raise ProtocolError(line)
                return  # Ignore telemetry already in flight before CSV_BEGIN.
            self.snapshot = int(match[1])
            if self.snapshot > 0xFFFFFFFF:
                raise ProtocolError('Invalid snapshot')
            self.started = True
            return
        if not self.header:
            if line != ','.join(COLUMNS):
                raise ProtocolError('Missing CSV column header')
            self.writer.writerow(COLUMNS)
            self.header = True
            return
        match = END.fullmatch(line)
        if match:
            self.skipped = int(match[2])
            if match[3] != 'OK':
                raise ProtocolError('Incomplete transfer: ' + match[3])
            if int(match[1]) != self.rows:
                raise ProtocolError('CSV row count mismatch')
            self.done = True
            return
        fields = next(csv.reader([line]))
        if len(fields) != len(COLUMNS):
            raise ProtocolError('Unexpected text or malformed CSV row: ' + line)
        try:
            sequence, session, uptime = [int(value) for value in fields[:3]]
            if any(not re.fullmatch(r'\d+', value) for value in fields[:3]):
                raise ValueError('Non-decimal identifier')
            if not (0 < sequence <= self.snapshot and 0 < session <= 0xFFFFFFFF and
                    0 <= uptime <= 0xFFFFFFFF):
                raise ValueError('Identifier outside bounds')
            if self.previous is not None and sequence >= self.previous:
                raise ValueError('Sequence is not strictly descending')
            if fields[6] not in ('0', '1') or fields[7] not in tuple(str(i) for i in range(8)):
                raise ValueError('Invalid flags')
            for value in fields[4:6]:
                if not re.fullmatch(r'\d+\.\d', value):
                    raise ValueError('Invalid measurement')
            if fields[8] == 'unknown':
                if fields[3] != 'UNCALIBRATED':
                    raise ValueError('Unknown time requires UNCALIBRATED')
            elif fields[8] in ('rtc', 'estimated'):
                if not re.fullmatch(r'\d{4} \d{2} \d{2} \d{2}:\d{2}:\d{2}', fields[3]):
                    raise ValueError('Invalid date format')
                date = datetime.strptime(fields[3], '%Y %m %d %H:%M:%S')
                if not 2020 <= date.year <= 2099:
                    raise ValueError('Date outside supported range')
            else:
                raise ValueError('Invalid time source')
        except ValueError as exc:
            raise ProtocolError(str(exc)) from exc
        self.writer.writerow(fields)
        self.previous = sequence
        self.rows += 1


def receive_export(port, stream, timeout=60.0):
    receiver = CsvReceiver(stream)
    pending = bytearray()
    last_data = time.monotonic()
    while not receiver.done:
        chunk = port.read(256)
        if chunk:
            last_data = time.monotonic()
            pending.extend(chunk)
            while b'\n' in pending:
                line, _, tail = pending.partition(b'\n')
                pending = bytearray(tail)
                try:
                    receiver.feed(line.removesuffix(b'\r').decode('ascii'))
                except UnicodeError as exc:
                    raise ProtocolError('Non-ASCII data inside serial transfer') from exc
                if receiver.done:
                    break  # Subsequent ENV traffic belongs to the normal console.
            if len(pending) > 256:
                raise ProtocolError('Oversized or unterminated serial line')
        if time.monotonic() - last_data >= timeout:
            raise TimeoutError('No serial data within the idle timeout')
    return receiver


def export_to_file(port, target, timeout=60.0):
    """Only a validated end marker promotes the .part file to the final CSV."""
    target = Path(target)
    partial = target.with_name(target.name + '.part')
    if target.exists() or partial.exists():
        raise FileExistsError('Output or .part file already exists; choose another filename')
    # Exclusive create protects a previous export even if another process races.
    with partial.open('x', encoding='utf-8-sig', newline='') as stream:
        try:
            port.write(b'log export\r\n')
            receiver = receive_export(port, stream, timeout)
        except (Exception, KeyboardInterrupt):
            try:
                port.write(b'log cancel\r\n')
            except Exception:
                pass
            raise
    # Refuse to overwrite a file created while the transfer was running.
    if target.exists():
        raise FileExistsError('Target appeared during export; partial file preserved')
    partial.rename(target)
    return receiver


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='Serial port, e.g. COM7')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--timeout', type=float, default=60.0, help='Idle timeout in seconds')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    try:
        import serial
    except ImportError:
        parser.exit(2, 'Install dependency first: python -m pip install pyserial\n')
    port = serial.Serial(port=None, baudrate=args.baud, timeout=0.2, write_timeout=2,
                         xonxoff=False, rtscts=False, dsrdtr=False)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        with port:
            port.reset_input_buffer()
            result = export_to_file(port, args.output, args.timeout)
        print(f'Saved {result.rows} rows to {args.output.resolve()} (newest first)')
        if result.skipped:
            print(f'Warning: skipped {result.skipped} corrupt storage slots; see project guide.')
        return 0
    except (Exception, KeyboardInterrupt) as exc:
        print(f'Export failed: {exc or "cancelled by user"}', file=sys.stderr)
        partial = args.output.with_name(args.output.name + '.part')
        if partial.exists():
            print(f'Incomplete file retained: {partial.resolve()}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
