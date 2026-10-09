import os, hashlib, random, sys
random.seed(7)

def benc(x):
    if isinstance(x, int): return b"i%de" % x
    if isinstance(x, bytes): return b"%d:%s" % (len(x), x)
    if isinstance(x, str): return benc(x.encode())
    if isinstance(x, list): return b"l" + b"".join(benc(i) for i in x) + b"e"
    if isinstance(x, dict):
        return b"d" + b"".join(benc(k) + benc(v) for k, v in sorted(x.items())) + b"e"

PL = 262144
def make(name, files, out, tracker):
    # files: list of (relpath, size)
    root = "/tmp/ft/src/" + name
    data = b""
    entries = []
    for rel, size in files:
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if not os.path.exists(p):
            with open(p, "wb") as f: f.write(bytes(random.getrandbits(8) for _ in range(size)))
        b = open(p, "rb").read()
        data += b
        entries.append({"length": len(b), "path": rel.split("/")})
    pieces = b"".join(hashlib.sha1(data[i:i+PL]).digest() for i in range(0, len(data), PL))
    info = {"name": name, "piece length": PL, "pieces": pieces}
    if len(files) == 1 and False: pass
    info["files"] = entries
    t = {"announce": tracker, "info": info}
    open(out, "wb").write(benc(t))
    print(name, len(data), "bytes", len(pieces)//20, "pieces", hashlib.sha1(benc(info)).hexdigest())

if __name__ == "__main__":
    tracker = "http://127.0.0.1:16969/announce"
    os.makedirs("/tmp/ft/torrents_src", exist_ok=True)
    make("Game One", [("a.bin", 2_000_000), ("b.bin", 1_300_000), ("sub/c.bin", 300_001)], "/tmp/ft/torrents_src/Game One.torrent", tracker)
    make("Small", [("only.bin", 700_000)], "/tmp/ft/torrents_src/Small.torrent", tracker)
