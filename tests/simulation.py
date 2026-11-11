"""Independent UDP fixture; Python is used only in development and CI."""
import hashlib
import hmac
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time

HOST = ("127.0.0.1", 47632)
KEY = secrets.token_bytes(32)
ZERO = bytes(16)
ENV = {**os.environ, "BRIGHTSYNC_TEST_KEY": KEY.hex()}
FORMAT = "!4sBBH16s16sI"


def packet(kind, sender, receiver=ZERO, value=0, key=KEY, version=2):
    body = struct.pack(FORMAT, b"BSYN", version, kind, 0, sender, receiver, value)
    return body + hmac.digest(key, body, hashlib.sha256)


def decode(data, kind):
    assert len(data) == 76
    body = data[:-32]
    assert hmac.compare_digest(hmac.digest(KEY, body, hashlib.sha256), data[-32:])
    magic, version, actual, reserved, sender, receiver, brightness = struct.unpack(FORMAT, body)
    assert (magic, version, actual, reserved) == (b"BSYN", 2, kind, 0)
    return sender, receiver, brightness


def silent(sock):
    old = sock.gettimeout()
    sock.settimeout(0.2)
    try:
        data, _ = sock.recvfrom(256)
    except TimeoutError:
        return
    finally:
        sock.settimeout(old)
    raise AssertionError(f"Unexpected UDP response: {len(data)} bytes")


def mac(exe):
    env = ENV
    for value, hundredths in [("52.375", 5238), ("0", 0), ("100", 10000)]:
        # Rebind as a fresh receiver; the second exchange uses different nonces.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(HOST)
            sock.settimeout(4)
            probe = subprocess.Popen([exe, "--probe", HOST[0], value], env=env)
            try:
                data, endpoint = sock.recvfrom(256)
                sender, old_receiver, brightness = decode(data, 1)
                if hundredths == 0:  # drop the first hello; the same process retries
                    stale_endpoint = endpoint
                    data, endpoint = sock.recvfrom(256)
                    new_sender, old_receiver, brightness = decode(data, 1)
                    assert sender != new_sender and endpoint != stale_endpoint
                    sock.sendto(packet(2, sender, secrets.token_bytes(16)), stale_endpoint)
                    sender = new_sender
                assert old_receiver == ZERO and brightness == 0
                challenge = secrets.token_bytes(16)
                sock.sendto(packet(2, sender, challenge, key=ZERO), endpoint)
                sock.sendto(packet(2, sender, challenge, version=1), endpoint)
                sock.sendto(packet(2, sender, challenge) + b"x", endpoint)
                sock.sendto(packet(2, ZERO, challenge), endpoint)
                silent(sock)
                sock.sendto(packet(2, sender, challenge), endpoint)
                data, _ = sock.recvfrom(256)
                assert decode(data, 3) == (sender, challenge, hundredths)
                sock.sendto(packet(4, sender, challenge, key=ZERO), endpoint)
                sock.sendto(packet(4, sender, secrets.token_bytes(16)), endpoint)
                time.sleep(0.15)
                assert probe.poll() is None
                sock.sendto(packet(4, sender, challenge), endpoint)
                assert probe.wait(timeout=3) == 0
            finally:
                if probe.poll() is None:
                    probe.kill()
                    probe.wait(timeout=3)
    for invalid in ("-1", "nan", "inf", "100.1", "1,000", "oops"):
        assert subprocess.run([exe, "--probe", HOST[0], invalid], env=env, timeout=5).returncode == 2


def windows(exe):
    with tempfile.TemporaryDirectory() as folder:
        trace = Path(folder) / "trace.txt"
        app = subprocess.Popen([exe, "--simulate"], env={**ENV, "BRIGHTSYNC_TEST_TRACE": str(trace)})
        try:
            def poll(value, malformed=False):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.settimeout(0.3)
                    client = secrets.token_bytes(16)
                    for _ in range(50):
                        assert app.poll() is None
                        sock.sendto(packet(1, client), HOST)
                        try:
                            challenge, _ = sock.recvfrom(256)
                            break
                        except ConnectionResetError as error:
                            # Winsock reports ICMP port-unreachable if the GUI process is still binding.
                            if getattr(error, "winerror", None) != 10054:
                                raise
                            time.sleep(0.1)
                        except TimeoutError:
                            continue
                    else:
                        raise AssertionError("No authenticated challenge from receiver")
                    sender, server, brightness = decode(challenge, 2)
                    assert sender == client and server != ZERO and brightness == 0
                    if malformed:
                        sock.sendto(packet(3, client, server, value, key=ZERO), HOST)
                        sock.sendto(packet(3, client, server, 10001), HOST)
                        sock.sendto(packet(3, client, server, value, version=1), HOST)
                        sock.sendto(packet(3, client, server, value) + b"x", HOST)
                        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
                            other.sendto(packet(3, client, server, value), HOST)
                            silent(other)
                        silent(sock)
                    good = packet(3, client, server, value)
                    sock.sendto(good, HOST)
                    sock.settimeout(3)
                    ack, _ = sock.recvfrom(256)
                    assert decode(ack, 4) == (client, server, 0)
                    sock.sendto(good, HOST)
                    silent(sock)

            def wait_for_levels(predicate, timeout, description):
                until = time.monotonic() + timeout
                while time.monotonic() < until:
                    try:
                        levels = [int(line.split()[1]) for line in trace.read_text().splitlines()
                                  if line.startswith("brightness ")]
                        if predicate(levels):
                            return
                    except (FileNotFoundError, PermissionError):
                        pass
                    time.sleep(0.1)
                raise AssertionError(description)

            poll(3900, malformed=True)
            poll(3900)  # new endpoint
            wait_for_levels(lambda levels: levels[-1:] == [39], 6, "The live ramp did not reach its target")
            wait_for_levels(lambda levels: levels[-2:] == [39, 40], 16, "The fallback did not apply after disconnect")
            poll(6000)
            wait_for_levels(lambda levels: levels[11:13] == [40, 41], 5,
                            "The live ramp did not resume after reconnect")
        finally:
            app.terminate()
            app.wait(timeout=5)
        lines = trace.read_text().splitlines()
        assert [x for x in lines if x.startswith("sample ")] == ["sample 3900", "sample 3900", "sample 6000"]
        levels = [int(x.split()[1]) for x in lines if x.startswith("brightness ")]
        assert levels[:13] == list(range(49, 38, -1)) + [40, 41]


if __name__ == "__main__":
    platform, executable = sys.argv[1:]
    {"mac": mac, "windows": windows}[platform](str(Path(executable).resolve()))
    print(f"{platform} UDP simulation passed")
