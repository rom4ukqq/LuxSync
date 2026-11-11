"""Drive the Swift pairing client over UDP with BoringSSL on the fixture side."""
import ctypes
import os
from pathlib import Path
import socket
import subprocess
import sys


def exercise(executable, library):
    crypto = ctypes.CDLL(library)
    crypto.bs_pake_start.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p]
    crypto.bs_pake_start.restype = ctypes.c_void_p
    crypto.bs_pake_finish.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p]
    crypto.bs_pake_finish.restype = ctypes.c_int
    crypto.bs_pake_free.argtypes = [ctypes.c_void_p]
    crypto.bs_pake_free.restype = None

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(("127.0.0.1", 47632))
        udp.settimeout(3)
        sender = subprocess.Popen(
            [executable, "--pair-probe", "127.0.0.1"],
            env={**os.environ, "BRIGHTSYNC_TEST_PIN": "abcd-2345"},
        )
        try:
            start, endpoint = udp.recvfrom(128)
            assert len(start) == 54 and start[:6] == b"BSPR\x01\x01"
            assert udp.recvfrom(128) == (start, endpoint)  # first attempt dropped
            nonce = start[6:22]
            offer = ctypes.create_string_buffer(32)
            context = crypto.bs_pake_start(0, b"ABCD2345", offer)
            assert context
            result = ctypes.create_string_buffer(96)
            try:
                assert crypto.bs_pake_finish(context, start[22:], nonce, result) == 1
            finally:
                crypto.bs_pake_free(context)
            udp.sendto(b"BSPR\x01\x02" + nonce + offer.raw, endpoint)
            confirm = b"BSPR\x01\x03" + nonce + result.raw[32:64]
            assert udp.recvfrom(128) == (confirm, endpoint)
            proof = result.raw[64:]
            udp.sendto(b"BSPR\x01\x04" + nonce + bytes([proof[0] ^ 1]) + proof[1:], endpoint)
            assert udp.recvfrom(128) == (confirm, endpoint)  # bogus proof ignored, confirm retried
            udp.sendto(b"BSPR\x01\x04" + nonce + proof, endpoint)
            assert sender.wait(timeout=3) == 0
        finally:
            if sender.poll() is None:
                sender.kill()
                sender.wait(timeout=3)
    print("macOS PIN pairing over UDP passed")


if __name__ == "__main__":
    exercise(*(str(Path(argument).resolve()) for argument in sys.argv[1:]))
