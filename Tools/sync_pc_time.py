"""Set the board RTC from the computer clock (UTC+8), using existing USART1.

Usage: python Tools/sync_pc_time.py --port COM5
Dependency: python -m pip install pyserial
Close the serial assistant before opening the same port here.
"""
import argparse
from datetime import datetime, timedelta, timezone
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='Serial port, e.g. COM5')
    parser.add_argument('--baud', type=int, default=115200)
    args = parser.parse_args()
    try:
        import serial
    except ImportError:
        parser.error('Install pyserial first: python -m pip install pyserial')

    # Configure before opening, avoiding default DTR/RTS assertion on adapters.
    port = serial.Serial(port=None, baudrate=args.baud, timeout=0.2, write_timeout=1)
    port.port = args.port
    port.dtr = False
    port.rts = False
    try:
        with port:
            port.reset_input_buffer()
            local = datetime.now(timezone(timedelta(hours=8))).strftime('%Y %m %d %H:%M:%S')
            command = 'time set ' + local
            print('TX:', command)
            port.write((command + '\n').encode('ascii'))
            # The acknowledgement now waits for the calibration anchor commit.
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                line = port.readline().decode('ascii', errors='replace').strip()
                if line.startswith('OK time set'):
                    print(line)
                    port.write(b'time get\n')
                    break
                if line.startswith('ERR'):
                    raise RuntimeError(line)
            else:
                raise RuntimeError('No time-set acknowledgement; check port, baud and firmware.')
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                line = port.readline().decode('ascii', errors='replace').strip()
                if line.startswith('TIME '):
                    if 'UNCALIBRATED' in line:
                        raise RuntimeError(line)
                    print(line)
                    return
            raise RuntimeError('Calibration ACK received, but time query timed out.')
    except (serial.SerialException, RuntimeError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
