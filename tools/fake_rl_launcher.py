"""fake_rl_launcher.py - play RecompLauncher's side of the Recomp Achievements
channel on Windows, to test src/port_achievements.c without RecompLauncher.

    python tools/fake_rl_launcher.py build-mingw\\CHDPS3Recomp.exe [seconds] [log.jsonl]

Creates a byte-mode named pipe (\\\\.\\pipe\\RecompLauncher-<pid>), starts the
game with RL_SESSION naming it, answers its hello with "quiet" (the launcher
shows the toasts) and records every message the game sends, one JSON per line,
to the log (default out/rl_channel.jsonl). The game is closed after `seconds`
(default 60) unless it ends first. Prints a summary at the end.

The SDK's own sdk/tests/fake_launcher.py uses a Unix socket and checks its own
test program's messages; this is the Windows pipe and this game.
"""
import ctypes, json, os, subprocess, sys, threading, time
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateNamedPipeW.restype = wintypes.HANDLE
k32.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
k32.ConnectNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, PIPE_READMODE_BYTE, PIPE_WAIT = 3, 0, 0, 0
INVALID = wintypes.HANDLE(-1).value


def main():
    exe = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    log_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join("out", "rl_channel.jsonl")
    name = r"\\.\pipe\RecompLauncher-%d" % os.getpid()
    pipe = k32.CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                2, 65536, 65536, 0, None)
    if pipe == INVALID:
        sys.exit("CreateNamedPipe failed: %d" % ctypes.get_last_error())
    got = []
    os.makedirs(os.path.dirname(log_path) or ".", exist_ok=True)
    log = open(log_path, "w", encoding="utf-8")

    def send(obj):
        data = (json.dumps(obj) + "\n").encode()
        n = wintypes.DWORD()
        k32.WriteFile(pipe, data, len(data), ctypes.byref(n), None)

    def serve():
        k32.ConnectNamedPipe(pipe, None)
        buf = ctypes.create_string_buffer(65536)
        acc = b""
        while True:
            n = wintypes.DWORD()
            if not k32.ReadFile(pipe, buf, len(buf), ctypes.byref(n), None) or n.value == 0:
                break
            acc += buf.raw[:n.value]
            while b"\n" in acc:
                line, acc = acc.split(b"\n", 1)
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                got.append(m)
                log.write(json.dumps(m) + "\n")
                log.flush()
                if m.get("t") == "hello":
                    send({"t": "quiet"})

    threading.Thread(target=serve, daemon=True).start()
    env = dict(os.environ, RL_SESSION=name)
    proc = subprocess.Popen([exe], env=env)
    try:
        proc.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        proc.terminate()
    time.sleep(0.5)
    k32.CloseHandle(pipe)
    log.close()
    types = {}
    for m in got:
        types[m.get("t")] = types.get(m.get("t"), 0) + 1
    print("messages:", types)
    hello = next((m for m in got if m.get("t") == "hello"), None)
    if hello:
        print("hello:", hello)
    nl = next((m for m in got if m.get("t") == "native_list"), None)
    if nl:
        print("native_list: %d achievements, first: %s" % (len(nl["achievements"]), nl["achievements"][0]))
    for m in got:
        if m.get("t") in ("native", "save_loaded", "casual"):
            print(m)


if __name__ == "__main__":
    main()
