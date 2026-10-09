import sys
p = sys.argv[1]
s = open(p).read()
if 'write_cost' in s:
    print("hook already present"); sys.exit(0)
old = '    bool ok = io_impl(index, pieceOffset, buf, len, isWrite);'
assert old in s, "anchor not found in " + p
hook = '''#ifdef HOST_TEST
    if (isWrite) {   // тесты: модель медленной флешки "задержка на вызов (мс) + время на объём (мкс на КБ)"
        FILE* wf = fopen("/tmp/ft/write_cost", "r");
        int perCall = 0, perKB = 0;
        if (wf) { if (fscanf(wf, "%d %d", &perCall, &perKB) != 2) { perCall = 0; perKB = 0; } fclose(wf); }
        if (perCall > 0 || perKB > 0) usleep((useconds_t)(perCall * 1000 + (len / 1024) * perKB));
    }
#endif
'''
s = s.replace(old, hook + old, 1)
open(p, 'w').write(s)
print("hook added to", p)
