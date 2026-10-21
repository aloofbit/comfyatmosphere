# Offline compile of one of comfyatmos's HLSL sources (perf branch, 2026-10-21).
#   python tools/perf/shader.py src/water.cpp kWetPsHlsl ps_3_0 [DEFINE ...]
# Joins the C++ string pieces of the variable (raw strings and macros named *_HLSL defined in the sources),
# compiles with fxc and prints the instruction count line and any errors. -d prints the whole disassembly.
import os, re, subprocess, sys, glob

FXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"

def literals(text):
    # Raw HLSL strings and plain "..." strings, in order, with the plain ones' escapes undone.
    parts = []
    for m in re.finditer(r'R"HLSL\((.*?)\)HLSL"|"((?:[^"\\\n]|\\.)*)"', text, re.S):
        if m.group(1) is not None:
            parts.append(m.group(1))
        else:
            parts.append(bytes(m.group(2), 'utf-8').decode('unicode_escape'))
    return ''.join(parts)

def macros(srcdir):
    out = {}
    for f in glob.glob(os.path.join(srcdir, '*.h')) + glob.glob(os.path.join(srcdir, '*.cpp')):
        t = open(f, encoding='utf-8').read()
        for m in re.finditer(r'#define\s+(\w+_HLSL)\s+((?:R"HLSL\(.*?\)HLSL"\s*)+)', t, re.S):
            out[m.group(1)] = ''.join(re.findall(r'R"HLSL\((.*?)\)HLSL"', m.group(2), re.S))
        # A macro of plain strings over lines ending in a backslash.
        for m in re.finditer(r'#define\s+(\w+_HLSL)\s*\\\n((?:[^\n]*\\\n)*[^\n]*\n)', t):
            out[m.group(1)] = literals(m.group(2))
    return out

def source(path, var):
    t = open(path, encoding='utf-8').read()
    m = re.search(r'\b' + var + r'(?:\[\])?\s*=\s*((?:\s|R"HLSL\(.*?\)HLSL"|\w+)+);', t, re.S)
    if not m:
        sys.exit('no ' + var)
    expr = m.group(1)
    mac = macros(os.path.dirname(path))
    parts = []
    for tok in re.finditer(r'R"HLSL\((.*?)\)HLSL"|(\w+_HLSL)', expr, re.S):
        parts.append(tok.group(1) if tok.group(1) is not None else mac.get(tok.group(2), ''))
    return ''.join(parts)

def main():
    a = [x for x in sys.argv[1:] if x != '-d']
    dis = '-d' in sys.argv
    path, var, prof = a[0], a[1], a[2]
    defs = a[3:]
    src = source(path, var)
    tmp = os.path.join(os.environ.get('TEMP', '.'), 'comfy_' + var + '.hlsl')
    open(tmp, 'w', encoding='utf-8').write(src)
    cmd = [FXC, '/nologo', '/T', prof, '/E', 'main', '/O3', '/Fc', tmp + '.asm', tmp]
    for d in defs:
        cmd[1:1] = ['/D', d + '=1']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit(1)
    asm = open(tmp + '.asm', encoding='utf-8', errors='replace').read()
    if dis:
        print(asm)
    for line in asm.splitlines():
        if 'instruction slots used' in line or 'approximately' in line:
            print(line.strip())
    w = [l for l in (r.stdout + r.stderr).splitlines() if 'warning' in l]
    for l in w[:10]:
        print(l)

main()
