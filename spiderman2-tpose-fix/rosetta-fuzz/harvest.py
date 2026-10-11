"""Collects every distinct instruction form the game's code uses, rewritten so it can be
run in isolation: memory operands point into a scratch buffer ([r15 + 0x40]), everything
that would jump, touch the stack or fault is left out. Writes tests.s (one label per test,
the instruction followed by ret) and forms.tsv (form, count in the game, sample).

  python3 harvest.py Spider-Man2.exe outdir"""
import sys, re, collections, pefile, capstone
from capstone import x86

pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

SKIP_MN = re.compile(r'^(j\w+|call|ret|retf|iret\w*|loop\w*|int\d?|int3|into|ud\d|hlt|syscall|sysret|sysenter|'
    r'cpuid|xgetbv|xsetbv|rdtsc|rdtscp|rdrand|rdseed|rdpmc|rdmsr|wrmsr|in|out|ins\w|outs\w|push\w*|pop\w*|enter|leave|'
    r'lods\w|stos\w|scas\w|xlatb?|div|idiv|xsave\w*|xrstor\w*|fxsave\w*|fxrstor\w*|lfence|sfence|mfence|'
    r'clflush\w*|prefetch\w*|pause|nop|endbr64|bound|lock|wait|fwait|cli|sti|std|cld|lahf|sahf|ldmxcsr|stmxcsr|vldmxcsr|vstmxcsr|'
    r'vzeroall|f\w+|emms|mov(sx|zx)d?|rdfsbase|rdgsbase|wrfsbase|wrgsbase|swapgs|lgdt|lidt|sgdt|sidt|ltr|str|invlpg|'
    r'cmpxchg16b|monitor|mwait|tzcnt_x)$')
# mov{sx,zx} are kept via the generic path below; listed above only by accident -> fix:
SKIP_MN = re.compile(SKIP_MN.pattern.replace('mov(sx|zx)d?|', ''))

def operand_key(insn, op):
    if op.type == x86.X86_OP_REG:
        n = insn.reg_name(op.reg)
        if n.startswith('ymm'): return 'ymm'
        if n.startswith('xmm'): return 'xmm'
        if n.startswith('k'): return 'k'
        return 'r%d' % (op.size * 8)
    if op.type == x86.X86_OP_IMM:
        return 'i%d' % (op.size * 8)
    if op.type == x86.X86_OP_MEM:
        return 'm%d' % (op.size * 8)
    return '?'

forms = collections.OrderedDict()
counts = collections.Counter()
skipped = collections.Counter()
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
for b, e in funcs:
    if not (text.VirtualAddress <= b < text.VirtualAddress + text.Misc_VirtualSize):
        continue
    for insn in md.disasm(img[b:e], base + b):
        mn = insn.mnemonic
        if insn.prefix[0] in (0xf0, 0xf2, 0xf3) and mn.startswith(('rep', 'lock')):
            pass
        string_op = mn in ('movsb', 'movsw', 'movsd', 'movsq', 'cmpsb', 'cmpsw', 'cmpsd', 'cmpsq') and not insn.op_str.count(',')
        if string_op and 'xmm' in insn.op_str:
            string_op = False
        if SKIP_MN.match(mn) or string_op or mn.startswith(('rep', 'lock ')) or 'rep' in insn.mnemonic.split()[0]:
            skipped[mn] += 1
            continue
        ops = insn.operands
        bad = False
        for op in ops:
            if op.type == x86.X86_OP_REG and insn.reg_name(op.reg) in ('rsp', 'esp', 'sp', 'spl', 'rip', 'r15', 'r15d', 'r15w', 'r15b'):
                bad = True
            # segment, MMX/x87, mask, control/debug registers aren't part of the tested state
            if op.type == x86.X86_OP_REG and re.match(r'^(cs|ds|es|fs|gs|ss|mm\d|st\(?\d|k\d|cr\d|dr\d|bnd\d)', insn.reg_name(op.reg)):
                bad = True
            if op.type == x86.X86_OP_MEM and (op.mem.segment != 0):
                bad = True
            if op.type == x86.X86_OP_MEM and op.mem.index != 0 and insn.reg_name(op.mem.index).startswith(('xmm', 'ymm')):
                bad = True
        # implicit stack users
        # implicit stack users (memory operands based on rsp are fine: they get rewritten)
        if any(insn.reg_name(r) in ('rsp', 'rip') for r in list(insn.regs_read) + list(insn.regs_write)):
            bad = True
        if mn in ('bt', 'bts', 'btr', 'btc') and any(o.type == x86.X86_OP_MEM for o in ops) and any(o.type == x86.X86_OP_REG for o in ops):
            bad = True     # register bit offset into memory can reach anywhere
        if bad:
            skipped[mn + ' (operands)'] += 1
            continue
        key = mn + ' ' + ','.join(operand_key(insn, o) for o in ops)
        counts[key] += 1
        if key in forms:
            continue
        s = insn.op_str
        if mn == 'lea':
            if 'rip' in s:
                continue
        else:
            s = re.sub(r'\[[^\]]*\]', '[r15 + 0x40]', s)
        forms[key] = (mn, s, insn.bytes.hex())

out = sys.argv[2]
with open(out + '/tests.s', 'w') as f:
    f.write('.intel_syntax noprefix\n.text\n')
    for i, (key, (mn, s, raw)) in enumerate(forms.items()):
        f.write('.globl t%d\nt%d:\n    %s %s\n    ret\n' % (i, i, mn, s))
with open(out + '/forms.tsv', 'w') as f:
    for i, (key, (mn, s, raw)) in enumerate(forms.items()):
        f.write('%d\t%s\t%d\t%s %s\t%s\n' % (i, key, counts[key], mn, s, raw))
print('forms:', len(forms), 'instructions considered:', sum(counts.values()))
print('top skipped:', skipped.most_common(25))
